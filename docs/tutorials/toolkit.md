# Toolkit: what is this file?

The toolkit is the suite's "open anything" helper. Today it does two things well:

- **inspect**: identify a file from its contents (not just its name) and say which wetwaresuite tool opens it;
- **hex**: show a file's raw bytes as hex and text.

It comes in two forms: as the `toolkit.*` commands inside the `wetwaresuite` binary, and as a small standalone binary, `wetwaresuite-toolkit`. There is no GUI yet; when it arrives, the toolkit's "open with" and hex views will call these same commands.

## Task 1: identify a mystery file

Downloads often arrive with useless names like `download.dat`. The toolkit reads the first bytes (the "magic number" most formats start with) and recognises the format regardless of the extension.

**CLI.**

```sh
$ wetwaresuite toolkit.inspect path=download.dat
format: MP3 audio
category: audio
opens in: audio
mime: audio/mpeg
size: 31171 bytes
```

The fields:

| Field | Meaning |
| --- | --- |
| `format` | what the file really is |
| `category` | `image`, `raw`, `vector`, `cad`, `3d`, `texture`, `audio`, `video`, `document`, `spreadsheet`, `archive`, `executable`, `binary`, ... |
| `opens in` | the wetwaresuite tool for it: `photo`, `paint`, `vector`, `cad`, `asset forge`, `audio`, `cut`, `office` or `toolkit`. Some of these are planned tools (see `hub list`); for those, [convert](convert.md) can often change the file into something you can use today. |
| `mime` | the MIME type, handy for web servers and scripts |
| `size` | file size in bytes |

It recognises common images (including camera RAW files from Canon, Nikon, Sony, Fujifilm, Olympus and Panasonic), PSD, Krita, OpenRaster and GIMP files, SVG, EPS and other vector formats, CAD and 3D formats (STEP, DWG, STL, glTF, FBX, Blender, ...), audio, video, PDF and office documents, archives (ZIP, 7z, RAR, gzip, xz, zstd, tar), fonts, SQLite databases and executables. When the content gives no clue, it falls back to the extension, and then to "Plain text" or "Unknown binary".

One limitation: Office files (`.docx`, `.xlsx`, `.pptx`) and OpenDocument files are ZIP archives inside. With their real extension they are identified correctly, but renamed to something else they show up as `ZIP archive`.

**Batch.** To label a whole folder, loop in the shell:

```sh
for f in downloads/*; do
  printf '%-30s ' "$f"
  wetwaresuite toolkit.inspect path="$f" | sed -n 's/^format: //p'
done
```

```
downloads/episode-12.wav       WAV audio
downloads/hero.jpg             JPEG image
downloads/report.docx          Word document
```

In a batch script the command is the same line: `toolkit.inspect path=downloads/file.dat`.

**Claude.**

> What is /home/me/Downloads/file.dat, and what can I open it with?

Claude calls `toolkit_inspect`, and may follow up with `toolkit_hex` to look at the header, or `convert_check` to see whether it can be converted into something useful.

## Task 2: look at the raw bytes

```sh
$ wetwaresuite toolkit.hex path=talk.mp4 length=32
00000000  00 00 00 20 66 74 79 70  69 73 6f 6d 00 00 02 00  |... ftypisom....|
00000010  69 73 6f 6d 69 73 6f 32  61 76 63 31 6d 70 34 31  |isomiso2avc1mp41|
```

Each row shows the offset (in hex), 16 bytes in hex, and the same bytes as text (`.` for anything unprintable). `offset` starts somewhere other than the beginning and `length` sets how many bytes to show (default 256, maximum 65536). Integers can be written in hex too:

```sh
wetwaresuite toolkit.hex path=talk.mp4 offset=0x10 length=16
```

This is useful for checking a file header by eye, spotting a truncated download (all zeros at the end), or confirming what `inspect` reported.

## The standalone `wetwaresuite-toolkit`

The build also produces `build/tools/toolkit/wetwaresuite-toolkit`, a small binary with only these two commands and a shorter syntax (positional arguments instead of `key=value`):

```sh
$ wetwaresuite-toolkit inspect talk.mp4
format: MP4 video
category: video
opens in: cut
mime: video/mp4
size: 2043957 bytes

$ wetwaresuite-toolkit hex talk.mp4 4 8
00000004  66 74 79 70 69 73 6f 6d                           |ftypisom|

$ wetwaresuite-toolkit --version
wetwaresuite-toolkit 0.1.0
```

`hex` takes `<file> [offset] [length]`. Exit codes match the main binary: 0 on success, 1 when the command fails (for example `cannot read nope: No such file or directory`), 2 for a usage error.

It also has its own MCP server, `wetwaresuite-toolkit mcp`, which offers just `toolkit_inspect` and `toolkit_hex` (plus the `undo`, `redo`, `history` and `documents` session tools). Use it if you only want Claude to identify files; otherwise `wetwaresuite mcp` includes these tools along with everything else. Setup is the same as in [mcp.md](mcp.md):

```sh
claude mcp add wetwaresuite-toolkit -- /path/to/wetwaresuite/build/tools/toolkit/wetwaresuite-toolkit mcp
```

You can also serve only the toolkit from the main binary with `wetwaresuite mcp --tools toolkit`.

## Command reference

### toolkit.inspect
Identify a file's format from its contents and say which tool opens it. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string | yes | path to the file |

### toolkit.hex
Show raw bytes as hex and ASCII. Read-only.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `path` | string | yes | | path to the file |
| `offset` | integer | no | 0 | byte offset to start at |
| `length` | integer | no | 256 | number of bytes, 1..65536 |
