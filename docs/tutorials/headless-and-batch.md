# Headless use and batch scripts

wetwaresuite is built to run without a screen. Every feature is a command, and commands can run one at a time from your shell, many at a time from a script, or unattended in CI and cron jobs. This tutorial covers all three.

If you have not used the CLI yet, read [getting started](getting-started.md) first: it explains `key=value` arguments, colours, lists and document ids.

## One-shot commands

Commands that read a file and write a file need no script at all:

```sh
wetwaresuite convert.file input=interview.wav output=interview.mp3 quality=80
wetwaresuite photo.batch input_dir=photos output_dir=web max_size=1600 quality=82
wetwaresuite toolkit.inspect path=download.dat
```

Anything that edits an open document over several steps (open, adjust, save) needs a batch script, because documents only live as long as the process that opened them.

## Your first batch script

A batch script is a plain text file with one command per line, written exactly as you would after `wetwaresuite run`. Save this as `fix-dark.txt`:

```text
# brighten an underexposed photo and save a web copy
photo.open path=dark.jpg -> pic
photo.levels doc=$pic auto=true
photo.adjust doc=$pic kind=exposure amount=0.5
photo.adjust doc=$pic kind=vibrance amount=20
photo.resize doc=$pic width=800
photo.save doc=$pic path=dark-fixed.jpg quality=85
echo done with $pic
```

Run it:

```sh
$ wetwaresuite batch fix-dark.txt
opened dark.jpg as img1
doc: img1
width: 1200
height: 800
layers: 1
auto contrast applied
...
saved dark-fixed.jpg
path: dark-fixed.jpg
bytes: 31531
done with img1
```

Each command prints what it did, just like `run`. The whole script runs in one process, so `img1` stays open from the first line to the last.

## Script syntax

### Lines and comments

- One command per line. Blank lines are ignored. Windows line endings are fine.
- `#` starts a comment when it is the first character of the line or comes right after a space or tab. A `#` inside a word, such as `color=#ff8800`, is a colour, not a comment.

```text
# a whole-line comment
paint.layer_add doc=$art name=Sky   # a trailing comment
paint.fill doc=$art x=5 y=5 color=#87ceeb
```

A comment starts at a `#` that begins a word and is not inside quotes, so hex colours like `color=#87ceeb` and names like `name="Take #1"` are left alone:

```text
paint.fill doc=$art x=5 y=5 color=#87ceeb   # sky blue
```

### Quoting and escapes

Values with spaces go in double quotes. A backslash before `"`, `$`, `#` or a space makes that character literal; any other backslash is kept as it is, so Windows paths like `C:\Users\me\photo.png` work unquoted.

```text
paint.layer_add doc=$art name="sky layer"
photo.save doc=$art path="out dir with space.png"
# a # inside quotes is just text
paint.layer_add doc=$art name="Take #1"
# \$ stops variable substitution
echo price is \$5
```

Single quotes are not special in scripts: use double quotes. An unclosed `"` is an error (`unterminated quote`).

### Capturing results with `-> name`

Put `-> name` (an arrow, a space, then a name) at the end of a line to remember the document id the command returned:

```text
photo.open path=photos/holiday-1.jpg -> hero
paint.new width=1200 height=630 -> card
```

The arrow and the name must be separate words: `->card` or a missing name are errors.

Capturing is meant for document ids. Commands that do not return a document (for example `photo.pick_color` or `convert.file`) store an empty value, so `$name` would then be replaced by nothing.

### Using variables with `$name`

`$name` is replaced by the captured value anywhere in a later line, including inside quoted values and paths:

```text
photo.open path=raw.jpg -> p
photo.resize doc=$p width=1080
echo finished $p
```

A `$word` that was never captured is left exactly as written. Longer names win, so `$img2` is not mistaken for `$img` followed by `2`.

### Built-in commands

| Line | What it does |
| --- | --- |
| `undo` | undo the last edit in this session (fails with `nothing to undo` when there is none) |
| `redo` | redo the last undone edit |
| `echo some text` | print the text, after `$name` substitution; handy for progress messages |

## Running scripts

| How | Command |
| --- | --- |
| from a file | `wetwaresuite batch script.txt` |
| from stdin | `wetwaresuite batch` or `wetwaresuite batch -` |
| keep going after a failing line | `wetwaresuite batch script.txt --keep-going` |

### Errors and `--keep-going`

By default a script stops at the first failing line. The error goes to stderr with the line number:

```sh
$ printf 'photo.new width=1 height=1\nbogus.cmd\necho after\n' | wetwaresuite batch
created img1
...
line 2: unknown command: bogus.cmd
```

With `--keep-going` every line runs, errors are still reported, and the exit code still tells you something went wrong:

```sh
$ printf 'photo.new width=1 height=1\nbogus.cmd\necho after\n' | wetwaresuite batch - --keep-going
created img1
...
line 2: unknown command: bogus.cmd
after
$ echo $?
1
```

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | every line succeeded |
| 1 | at least one line failed (with or without `--keep-going`) |
| 2 | the script file could not be opened (`cannot open missing.txt`) |

## A realistic pipeline

This script turns a folder of raw material into a week's blog assets: a hero image in two sizes, a social card painted from scratch, a PDF of a Word report, and an MP3 of a podcast episode. Save it as `publish.txt` next to a `release/` folder that holds `hero.jpg`, `report.docx` and `episode-12.wav`.

```text
# publish.txt: build this week's blog assets from the raw files in release/
# run with: mkdir -p out && wetwaresuite batch publish.txt

# 1. hero image: crop to 16:9, brighten a touch, save a large and a small copy
photo.open path=release/hero.jpg -> hero
photo.crop doc=$hero x=0 y=156 width=3000 height=1688
photo.levels doc=$hero auto=true
photo.adjust doc=$hero kind=vibrance amount=15
photo.resize doc=$hero width=1920
photo.save doc=$hero path=out/hero-1920.jpg quality=85
photo.resize doc=$hero width=640
photo.effect doc=$hero kind=sharpen amount=1
photo.save doc=$hero path=out/hero-640.jpg quality=80
photo.close doc=$hero

# 2. social card: 1200x630, gradient background, a title bar and a badge
paint.new width=1200 height=630 -> card
paint.gradient doc=$card layer=0 x1=0 y1=0 x2=1200 y2=630 from=#0f2027 to=#2c5364
paint.shape doc=$card kind=rect x=60 y=420 width=1080 height=140 color=#ffffff22
paint.shape doc=$card kind=ellipse x=1010 y=60 width=130 height=130 color=gold
paint.stroke doc=$card brush=ink color=white size=14 points=90,500,400,500 pressures=0.3,1
photo.save doc=$card path=out/social-card.png

# 3. documents and audio (these need LibreOffice and ffmpeg; see convert.md)
convert.file input=release/report.docx output=out/report.pdf overwrite=true
convert.file input=release/episode-12.wav output=out/episode-12.mp3 quality=70 overwrite=true

echo all done: $hero and $card
```

```sh
$ mkdir -p out && wetwaresuite batch publish.txt > publish.log
$ tail -1 publish.log
all done: img1 and img2
$ ls out
episode-12.mp3  hero-1920.jpg  hero-640.jpg  report.pdf  social-card.png
```

On a laptop this whole script runs in about a second and a half, most of it spent in LibreOffice.

A few habits that make scripts safe to run again and again:

- **Output folders are created for you** by `photo.save`, `convert.file` and `photo.batch`.
- **Say `overwrite=true` on conversions you re-run.** `convert.file` refuses to replace an existing file unless you ask.
- **Re-running `photo.batch` is cheap.** Results that already exist are skipped unless you pass `overwrite=true`, and it never replaces a source image, even when `output_dir` is the input folder.
- **Close documents you are done with** (`photo.close`) in long scripts to free memory.

## Generating scripts from the shell

Because a script can come from stdin, a shell loop can build one per file. Here every photo is cropped square and resized for Instagram:

```sh
mkdir -p square
for f in photos/*.jpg; do
  name=$(basename "$f" .jpg)
  wetwaresuite batch <<EOF || echo "failed on $f" >&2
photo.open path="$f" -> p
photo.crop doc=\$p x=500 y=0 width=2000 height=2000
photo.resize doc=\$p width=1080
photo.save doc=\$p path="square/$name.jpg" quality=85
EOF
done
```

Inside an unquoted heredoc (`<<EOF`) the shell expands `$f` and `$name` for you, so the wetwaresuite variable has to be written `\$p` to reach the script intact. If you do not need shell variables, quote the heredoc (`<<'EOF'`) and write `$p` normally.

For whole-folder jobs, prefer the built-in folder commands, which are faster than a loop: `photo.batch` (resize, auto contrast, effects) and `convert.batch` (format changes).

## Running in CI

The binary has no GUI dependencies, so it runs on any build agent. A GitHub Actions job that builds the suite and regenerates assets on every push might look like this:

```yaml
name: assets
on: [push]
jobs:
  build-assets:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build wetwaresuite
        run: |
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build -j
      - name: Optional backends for audio, video and documents
        run: sudo apt-get update && sudo apt-get install -y ffmpeg libreoffice-writer-nogui pandoc
      - name: Generate assets
        run: |
          mkdir -p out
          ./build/apps/wetwaresuite/wetwaresuite batch publish.txt
      - uses: actions/upload-artifact@v4
        with:
          name: assets
          path: out/
```

A failing line makes `wetwaresuite batch` exit with 1, which fails the step. If you would rather collect every problem in one run, add `--keep-going`: the job still fails, but only after all lines have been tried.

To check up front that a machine has the backends a script needs, use `convert.check` (exit code 1 when no converter exists) or read `convert.formats`:

```sh
wetwaresuite convert.check input=release/report.docx to=pdf
```

## Running from cron

A nightly job that resizes the day's uploads and stays quiet unless something fails:

```cron
# m h  dom mon dow  command
30 2  *   *   *    cd /srv/site && /opt/wetwaresuite/wetwaresuite photo.batch input_dir=uploads output_dir=public/img max_size=1600 quality=82 > /dev/null 2>> logs/wetwaresuite.log
```

Or wrap a script so that output is only shown (and mailed by cron) on failure:

```sh
#!/bin/sh
cd /srv/site || exit 1
wetwaresuite batch publish.txt > /dev/null 2> /tmp/wetwaresuite-errors.log || {
  echo "publish failed:"
  cat /tmp/wetwaresuite-errors.log
  exit 1
}
```

## Where next

- [Using wetwaresuite with Claude (MCP)](mcp.md): the same commands, driven by Claude.
- Per tool: [photo](photo.md), [paint](paint.md), [vector](vector.md), [convert](convert.md), [toolkit](toolkit.md).
