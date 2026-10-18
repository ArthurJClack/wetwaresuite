# Cut: edit video

`cut` is a video editor without a screen. You build a **timeline** out of clips, stills, titles and music, look at single frames to check it, and render it to MP4, WebM, MOV, a GIF or just the sound. For quick jobs on one file (trim it, join two, make a GIF, burn in subtitles) there are one-shot commands that need no timeline at all:

```sh
wetwaresuite cut.gif input=interview.mp4 output=wave.gif start=3 duration=2.5
```

There is no GUI yet; the GUI that comes later will call these same commands. Each task below shows the CLI way, the batch-script way and the "ask Claude" way. Every CLI and batch example here was run against the real binary (outputs are copied from those runs; long `key: value` lists are shortened with `...`).

## What you need: ffmpeg

`cut` plans the edit itself and hands the decoding and encoding to [ffmpeg](https://ffmpeg.org), which must be installed and on your `PATH`:

| System | Install |
| --- | --- |
| Debian, Ubuntu | `sudo apt install ffmpeg` |
| Fedora | `sudo dnf install ffmpeg` (from RPM Fusion) |
| macOS (Homebrew) | `brew install ffmpeg` |
| Windows | `winget install ffmpeg` |

`ffprobe` comes with it. Without ffmpeg, the commands that need it fail with that advice instead of doing something half-way:

```sh
$ wetwaresuite cut.gif input=interview.mp4 output=x.gif
video editing needs ffmpeg, which isn't installed (or isn't on PATH). Install it, it's free: Debian/Ubuntu `sudo apt install ffmpeg`, macOS `brew install ffmpeg`, Windows `winget install ffmpeg`.
```

Creating, editing, saving and opening timelines works without ffmpeg (except adding media files, which are measured with ffprobe).

## How it works

- **A timeline** is a document with an id like `cut1`. It has a frame size (default 1920x1080), a frame rate (default 30) and a background colour, set with `cut.new` or later with `cut.settings`. Its length is wherever the last clip ends.
- **Tracks.** Video tracks are `v1`, `v2`, ...: `v1` is the bottom layer and every higher track is drawn on top of it, which is how picture-in-picture, logos and titles work. Audio tracks are `a1`, `a2`, ...; everything on every track is mixed. Tracks appear when you first put something on them.
- **Clips** have ids `c1`, `c2`, ... (one counter per timeline, whatever the kind):
  - `video`: a part of a video file, from source time `in` to `out` (seconds), placed at `start` on the timeline. Its sound comes with it.
  - `image`: a still for `duration` seconds (PNG transparency is kept).
  - `title`: text drawn over everything below it.
  - `audio`: a part of a sound file (or of a video's sound) on an audio track.
- **Times are seconds.** `start` is timeline time, `in`/`out` are source time. With `speed=2` a clip plays its source twice as fast, so 10 s of source fills 5 s of timeline.
- **The picture box.** A clip fills the whole frame unless you give it a `width` (and/or `height`), which makes it a box at `x`,`y` (pixels from the top-left). `fit` says how the picture fills its box: `fit` letterboxes it (the default), `fill` crops it to fill, `stretch` distorts it. `crop=x,y,w,h` cuts the source first.
- **Transitions** blend a clip into the one before it on the same track. The later clip slides back so the two overlap by the transition's length (later clips move with it), and the sound crossfades too.
- **Every edit is undoable** with `undo` (in batch scripts and over MCP), one step per command. Opening, previewing, rendering and the one-shot file commands don't change the timeline and stay out of the history.
- **Rendering builds one ffmpeg command** with one filter graph for the whole edit: each track's clips (trimmed, sped up, scaled into their box, coloured, faded, chained through `xfade` for transitions) are laid over a background, titles are drawn with `drawtext`, and all sound is delayed into place and mixed. If the timeline is nothing but back-to-back cuts of the same kind of file at the timeline's size, it is joined by **stream copy** instead: no re-encoding, so it is instant and lossless.
- **Previews** render one frame. Only the clips on screen at that moment are decoded, so a preview of minute 40 is as quick as one of second 1.

Clip ids and timelines live as long as the process: for the CLI that's one command, so editing a timeline step by step happens in a batch script, over MCP, or by saving it with `cut.save` (a `.wwcut` file) and reopening it later with `cut.open`.

The sample files in the tasks are `interview.mp4` (40 s, 1920x1080, with sound), `broll.mp4` (10 s of cutaway footage), `screen.mp4` (a 20 s screen recording, no sound), `webcam.mp4` (20 s, 640x480, with sound), `music.mp3` (60 s), `logo.png` (400x400) and `interview.srt` (subtitles).

### Look at a file first

```sh
$ wetwaresuite cut.probe path=interview.mp4
interview.mp4: mov,mp4,m4a,3gp,3g2,mj2, 40s
  stream 0: video h264 1920x1080 30fps
  stream 1: audio aac 44100Hz 1ch
path: interview.mp4
duration: 40
format: mov,mp4,m4a,3gp,3g2,mj2
has_video: true
has_audio: true
still: false
width: 1920
height: 1080
fps: 30
video_codec: h264
audio_codec: aac
streams: 2
```

## Task 1: a talking-head video into a short, with a title and music

You have a 40 s landscape interview and want a vertical 1080x1920 short of the two best moments (2-9 s and 15-22 s), with a crossfade between them, a title at the top for the first three seconds and quiet music underneath.

**Batch.** This is the natural way: one script, one process, the timeline built step by step. `-> short` stores the new timeline's id (`cut1`) and `$short` uses it; `-> part2` stores a clip id.

```text
# short.txt: a 1080x1920 short from a landscape interview
cut.new width=1080 height=1920 fps=30 -> short
# the good bits: 2-9s and 15-22s, cropped to fill the tall frame
cut.add_clip doc=$short path=interview.mp4 in=2 out=9 fit=fill -> part1
cut.add_clip doc=$short path=interview.mp4 in=15 out=22 fit=fill -> part2
cut.transition doc=$short clip=$part2 type=crossfade duration=0.5
# a title for the first few seconds
cut.add_title doc=$short text="3 ways to edit faster" start=0.3 duration=3 position=top font_size=90 box=true fade_in=0.3 fade_out=0.3
# music under the voice, faded in and out
cut.add_audio doc=$short path=music.mp3 volume=0.25 fade_in=1 fade_out=2 duration=13.5
cut.info doc=$short
cut.render doc=$short output=short-frame.png at=1.5 width=540
cut.render doc=$short output=short.mp4
cut.save doc=$short path=short.wwcut
```

```sh
$ wetwaresuite batch short.txt
created cut1 (1080x1920 30fps, 0 tracks, 0 clips, 0.00s)
...
added clip c1 on v1 at 0s (timeline 7s)
...
added clip c2 on v1 at 7s (timeline 14s)
...
crossfade from c1 to c2 over 0.5s (c2 now starts at 6.5s)
...
added title c3 on v2 at 0.3s (timeline 13.5s)
...
added audio c4 on a1 at 0s (timeline 13.5s)
...
cut1: 1080x1920 30fps, background black, 13.5s
v1 (video)
  c1 video interview.mp4 at 0-7s (7s) source 2-9 fit=fill
  c2 video interview.mp4 at 6.5-13.5s (7s) source 15-22 fit=fill transition=crossfade 0.5s from c1
v2 (video)
  c3 title "3 ways to edit faster" at 0.3-3.3s (3s) fade_in=0.3 fade_out=0.3 size=90 color=white position=top box=black@0.5
a1 (audio)
  c4 audio music.mp3 at 0-13.5s (13.5s) source 0-13.5 volume=0.25 fade_in=1 fade_out=2
...
rendered short-frame.png
...
width: 540
height: 960
rendered short.mp4
short.mp4
output: short.mp4
bytes: 4231517
duration: 13.5
width: 1080
height: 1920
mode: encode
saved short.wwcut
```

What happened:

- A clip without `start` goes at the end of its track, so `c2` landed at 7 s; the crossfade then pulled it back to 6.5 s so the two overlap by half a second. The timeline got 0.5 s shorter.
- `fit=fill` scales the landscape picture up until it covers the tall frame and crops the sides. Use `fit=fit` (the default) to see the whole picture with bars above and below, or `crop=` to choose which part to keep.
- `cut.add_title` without `track` puts titles on a track above everything else (here `v2`). `box=true` draws a half-transparent box behind the text so it reads on any background.
- The interview's own sound plays at full volume; the music sits at a quarter under it. `cut.render` writing a `.png` renders one frame (`at=1.5`), handy to check the layout before encoding.
- `short.wwcut` is a small JSON file. It points at the media by absolute path; it doesn't copy them. Timelines saved before the rename (`.achcut`) still open with `cut.open`.

**CLI.** A timeline only lives inside one process, so from the shell you pipe the lines into `wetwaresuite batch` (or keep them in a file as above). Re-rendering the saved project in another format is two lines:

```sh
$ printf '%s\n' 'cut.open path=short.wwcut -> s' 'cut.render doc=$s output=short.webm quality=draft' | wetwaresuite batch
opened cut1 (1080x1920 30fps, 3 tracks, 4 clips, 13.50s, short.wwcut)
...
rendered short.webm
short.webm
output: short.webm
bytes: 3209041
duration: 13.508
width: 1080
height: 1920
mode: encode
```

**Claude.**

> Make a vertical short (1080x1920) from ~/talks/interview.mp4: keep 2-9 s and 15-22 s with a quick crossfade, put "3 ways to edit faster" at the top for the first 3 seconds, and add ~/music/music.mp3 quietly underneath. Show me a frame before you render.

Claude calls `cut_probe` to see the file, `cut_new` with `width=1080 height=1920`, `cut_add_clip` twice (`in`/`out`, `fit=fill`), `cut_transition` on the second clip, `cut_add_title`, `cut_add_audio` with `volume` around 0.25, then `cut_preview` at a few times (it gets the frames as images, so it can check that the title fits and the crop keeps the face), and finally `cut_render`. If something looks off it fixes it with `cut_set_clip` or `undo` and looks again.

## Task 2: picture-in-picture

A screen recording with the presenter's webcam in the bottom-right corner, cropped square, with a thin orange frame around it.

**Batch.**

```text
# pip.txt: a screen recording with the presenter's webcam in the corner
cut.new -> tut
cut.add_clip doc=$tut path=screen.mp4 out=12 -> screen
# the webcam on v2: crop it square, shrink it to 360x360, bottom right, fade it in
cut.add_clip doc=$tut path=webcam.mp4 track=v2 start=0 out=12 crop=80,0,480,480 width=360 x=1520 y=680 fade_in=0.5 -> cam
# a thin frame around it: an orange square one layer below
cut.add_image doc=$tut path=logo.png track=v2 start=0 duration=12 x=1510 y=670 width=380 height=380 fit=stretch
cut.move_clip doc=$tut clip=$cam track=v3
cut.info doc=$tut
cut.render doc=$tut output=pip-frame.png at=4 width=960
cut.render doc=$tut output=pip.mp4
```

```sh
$ wetwaresuite batch pip.txt
...
moved c2 to v3 at 0s (timeline 12s)
...
cut1: 1920x1080 30fps, background black, 12s
v1 (video)
  c1 video screen.mp4 at 0-12s (12s) source 0-12 (no sound)
v2 (video)
  c3 image logo.png at 0-12s (12s) box=1510,670 380x380 fit=stretch
v3 (video)
  c2 video webcam.mp4 at 0-12s (12s) source 0-12 fade_in=0.5 box=1520,680 360x0 crop=80,0,480,480
...
rendered pip.mp4
```

- `width=360` alone keeps the picture's shape: the crop is 480x480, so the box is 360x360 (`info` shows `360x0` because only the width was given).
- `crop` is in the source's pixels, applied before scaling: `80,0,480,480` keeps a square from the middle of the 640x480 webcam.
- Layers are tracks: the frame is on `v2`, so the webcam had to move above it to `v3` (`cut.move_clip ... track=v3`). The webcam's sound plays; the screen recording has none.
- `opacity=0.8` would make the webcam see-through; `fade_out` fades it away at the end.

**CLI.** The same thing without the frame, piped in one go:

```sh
$ printf '%s\n' 'cut.new -> t' \
  'cut.add_clip doc=$t path=screen.mp4 out=12' \
  'cut.add_clip doc=$t path=webcam.mp4 track=v2 out=12 width=480 x=1400 y=560' \
  'cut.render doc=$t output=pip-quick.mp4' | wetwaresuite batch
...
rendered pip-quick.mp4
...
mode: encode
```

**Claude.**

> Put ~/rec/webcam.mp4 in the bottom-right corner of ~/rec/screen.mp4, about a fifth of the width, cropped to a square, and render the first 12 seconds.

Claude uses `cut_add_clip` twice (the second with `track=v2`, `width`, `x`, `y`, `crop`), checks the corner with `cut_preview`, and adjusts the position with `cut_set_clip` if the webcam covers something important.

## Task 3: crossfades and other transitions

Three shots and a closing logo, each joined differently.

**Batch.**

```text
# xfade.txt: three shots joined by different transitions
cut.new -> tl
cut.add_clip doc=$tl path=interview.mp4 in=0 out=5 -> a
cut.add_clip doc=$tl path=broll.mp4 in=0 out=5 -> b
cut.add_clip doc=$tl path=interview.mp4 in=20 out=25 -> c
cut.add_image doc=$tl path=logo.png duration=3 -> d
cut.transition doc=$tl clip=$b type=crossfade duration=1
cut.transition doc=$tl clip=$c type=wipeleft duration=0.75
cut.transition doc=$tl clip=$d type=fadeblack duration=1
cut.info doc=$tl
cut.render doc=$tl output=xfade-frame.png at=4.5 width=640
cut.render doc=$tl output=transitions.mp4
```

```sh
$ wetwaresuite batch xfade.txt
...
crossfade from c1 to c2 over 1s (c2 now starts at 4s)
...
wipeleft from c2 to c3 over 0.75s (c3 now starts at 8.25s)
...
fadeblack from c3 to c4 over 1s (c4 now starts at 12.25s)
...
cut1: 1920x1080 30fps, background black, 15.25s
v1 (video)
  c1 video interview.mp4 at 0-5s (5s) source 0-5
  c2 video broll.mp4 at 4-9s (5s) source 0-5 transition=crossfade 1s from c1
  c3 video interview.mp4 at 8.25-13.25s (5s) source 20-25 transition=wipeleft 0.75s from c2
  c4 image logo.png at 12.25-15.25s (3s) transition=fadeblack 1s from c3
...
rendered transitions.mp4
```

- Each `cut.transition` names the **later** clip. It moves that clip back to overlap the one before by `duration`, and everything after it on the track moves too: 18 s of clips became 15.25 s.
- The frame at 4.5 s is halfway through the crossfade. To check a whole render at a glance, make a contact sheet of it (task 6):

```sh
$ wetwaresuite cut.thumbnails input=transitions.mp4 output=transitions-sheet.jpg columns=6 rows=3 width=200
contact sheet of transitions.mp4: 6x3 frames in transitions-sheet.jpg
transitions-sheet.jpg
output: transitions-sheet.jpg
bytes: 66956
width: 1228
height: 352
```

- Transitions available: `crossfade`, `dissolve` (a grainy dissolve), `fadeblack` (fade through black), `fadewhite`, `wipeleft`, `wiperight`, `wipeup`, `wipedown`, `slideleft`, `slideright`, `slideup`, `slidedown`, `circleopen`, `circleclose`, `radial`, `pixelize`, `smoothleft`, `smoothright`, `distance`. `type=none` removes one (the clip moves back to where the other one ends).
- A transition can't be longer than either clip. If you later move a clip so the two no longer overlap, `info` shows the transition as `inactive` and the render just cuts.
- To fade the very first clip in from black or the last out to black, use `fade_in`/`fade_out` on the clip (they fade the sound too): `cut.set_clip doc=$tl clip=$a fade_in=1`.

**CLI.** Two files with a crossfade, piped:

```sh
printf '%s\n' 'cut.new -> t' 'cut.add_clip doc=$t path=intro.mp4' 'cut.add_clip doc=$t path=broll.mp4 -> b' \
  'cut.transition doc=$t clip=$b duration=1' 'cut.render doc=$t output=intro-broll.mp4' | wetwaresuite batch
```

(Without a transition, `cut.concat_files` joins files in one command; see task 6.)

**Claude.**

> Join clip1.mp4, clip2.mp4 and clip3.mp4 with one-second crossfades and fade the end to black.

Claude adds the three clips with `cut_add_clip`, calls `cut_transition` on the second and third, sets `fade_out=1` on the last with `cut_set_clip`, looks at a frame inside each transition with `cut_preview`, and renders with `cut_render`.

## Task 4: make a GIF

**CLI.** The one-shot `cut.gif` takes a piece of any video and makes a GIF with a proper palette (good colours, small file):

```sh
$ wetwaresuite cut.gif input=interview.mp4 output=wave.gif start=3 duration=2.5 fps=12 width=480
made wave.gif from 3-5.5s of interview.mp4
wave.gif
output: wave.gif
bytes: 415660
duration: 2.5
width: 480
height: 270
```

**Batch.** For a GIF with a caption, build a small timeline and render it to `.gif`:

```text
# gif.txt: a captioned reaction GIF: 2.5 seconds of the interview with a title
cut.new width=640 height=360 fps=15 -> g
cut.add_clip doc=$g path=interview.mp4 in=3 out=5.5
cut.add_title doc=$g text="wait for it..." start=0 duration=2.5 position=bottom font_size=40 color=yellow
cut.render doc=$g output=reaction.gif gif_fps=12 gif_width=480
```

```sh
$ wetwaresuite batch gif.txt
...
rendered reaction.gif
reaction.gif
output: reaction.gif
bytes: 434757
duration: 2.54
width: 480
height: 270
mode: encode
```

GIFs get big fast: keep them short, small (`width`/`gif_width` 320 to 480) and at 10 to 15 frames per second. They have no sound.

**Claude.**

> Make a 480 px wide GIF of 0:03 to 0:05.5 of interview.mp4 with "wait for it..." in yellow at the bottom.

Claude uses `cut_gif` for a plain GIF; for the caption it builds a timeline (`cut_new`, `cut_add_clip`, `cut_add_title`) and calls `cut_render` with a `.gif` output.

## Task 5: burn in subtitles

"Burning in" draws the subtitles into the picture, so they show on every player and site (unlike a separate `.srt`).

**CLI.**

```sh
$ wetwaresuite cut.subtitles input=interview.mp4 subtitles=interview.srt output=captioned.mp4 font_size=24
burned interview.srt into captioned.mp4
captioned.mp4
output: captioned.mp4
bytes: 25865806
duration: 40
width: 1920
height: 1080
```

- `font_size` is in libass units (about pixels on a 288-line-high canvas, scaled to the video), so 24 is large and 16 is the usual size. `color=#ffff00` makes `.srt` text yellow. `.ass` files keep their own styling.
- The sound is copied, the picture is re-encoded (H.264, `quality=draft|good|best`).

**Batch.** Any timeline render can burn subtitles over the finished edit with `subtitles=`. The times in the file are then **timeline** times, not the source's:

```text
cut.open path=short.wwcut -> s
cut.render doc=$s output=short-captioned.mp4 subtitles=interview.srt
```

```sh
$ printf '%s\n' 'cut.open path=short.wwcut -> s' 'cut.render doc=$s output=short-captioned.mp4 subtitles=interview.srt' | wetwaresuite batch
opened cut1 (1080x1920 30fps, 3 tracks, 4 clips, 13.50s, short.wwcut)
...
rendered short-captioned.mp4
...
duration: 13.5
```

**Claude.**

> Burn interview.srt into interview.mp4, a bit bigger than normal and in yellow.

Claude calls `cut_subtitles` with `font_size` and `color=#ffff00`, then `cut_thumbnails` with `preview=true` on the result to see a few frames with the captions.

## Task 6: quick jobs on single files

These take files and write files; no timeline needed. Outputs that exist are refused unless you pass `overwrite=true`:

```sh
$ wetwaresuite cut.trim_file input=interview.mp4 output=intro.mp4 start=2 end=9
intro.mp4 already exists (pass overwrite=true to replace it)
```

**Trim.** Frame-exact by default (re-encodes); `copy=true` is instant and lossless but starts at the keyframe before `start`, so the piece can be a little longer:

```sh
$ wetwaresuite cut.trim_file input=interview.mp4 output=intro.mp4 start=2 end=9
trimmed 2-9s of interview.mp4 into intro.mp4
intro.mp4
output: intro.mp4
bytes: 4496736
duration: 7
...
$ wetwaresuite cut.trim_file input=interview.mp4 output=intro-fast.mp4 start=2 end=9 copy=true
trimmed 2-9s of interview.mp4 into intro-fast.mp4
...
duration: 7.13867
```

**Join.** Files with the same codecs, size and frame rate are joined by stream copy; anything else is re-encoded to the first file's size and frame rate, letterboxed:

```sh
$ wetwaresuite cut.concat_files inputs=intro.mp4,broll.mp4 output=joined.mp4
joined 2 files into joined.mp4 by stream copy
...
duration: 17.0232
mode: copy
$ wetwaresuite cut.concat_files inputs=interview.mp4,webcam.mp4 output=mixed.mp4 quality=draft
joined 2 files into mixed.mp4 (re-encoded)
...
duration: 60
mode: encode
```

Separate the files with commas; if a name contains a comma, separate them with `|` or new lines instead.

**Sound.** Take the sound out (copied without re-encoding when the format allows, e.g. AAC into `.m4a`), or remove it:

```sh
$ wetwaresuite cut.extract_audio input=interview.mp4 output=voice.m4a
extracted the sound of interview.mp4 to voice.m4a
...
duration: 40
$ wetwaresuite cut.extract_audio input=interview.mp4 output=voice.mp3
extracted the sound of interview.mp4 to voice.mp3
...
$ wetwaresuite cut.mute input=broll.mp4 output=broll-silent.mp4
copied broll.mp4 without sound to broll-silent.mp4
...
```

**Speed.** The sound speeds up or slows down without changing pitch:

```sh
$ wetwaresuite cut.speed input=screen.mp4 output=screen-4x.mp4 factor=4
changed the speed of screen.mp4 by 4x into screen-4x.mp4
...
duration: 5
```

**Contact sheet.** A grid of frames spread evenly through a video, to see what's in it:

```sh
$ wetwaresuite cut.thumbnails input=interview.mp4 output=sheet.jpg columns=5 rows=4 width=240
contact sheet of interview.mp4: 5x4 frames in sheet.jpg
sheet.jpg
output: sheet.jpg
bytes: 100962
width: 1224
height: 564
```

**Batch.** The same commands work in scripts, e.g. a folder's worth of intros and contact sheets:

```text
# quick.txt
cut.trim_file input=day1.mp4 output=out/day1-intro.mp4 start=0 duration=15
cut.thumbnails input=day1.mp4 output=out/day1-sheet.jpg
cut.trim_file input=day2.mp4 output=out/day2-intro.mp4 start=0 duration=15
cut.thumbnails input=day2.mp4 output=out/day2-sheet.jpg
```

or a shell loop over files: `for f in *.mp4; do wetwaresuite cut.thumbnails input="$f" output="sheets/${f%.mp4}.jpg"; done`.

**Claude.**

> What's in ~/Videos/day1.mp4? Then cut 0:42 to 1:30 out of it into highlight.mp4.

Claude calls `cut_probe`, then `cut_thumbnails` with `preview=true` (it sees the sheet), then `cut_trim_file` with `start=42 end=90`.

## Task 7: colour

`cut.filter` adjusts brightness, contrast, saturation and gamma, makes clips black and white, or applies a `.cube` LUT colour grade. Without `clip` it changes every video and image clip.

**Batch.**

```text
# colour.txt: grade a whole edit, then make one shot black and white
cut.new -> tl
cut.add_clip doc=$tl path=interview.mp4 out=6 -> a
cut.add_clip doc=$tl path=broll.mp4 out=4 -> b
cut.filter doc=$tl brightness=0.05 contrast=1.15 saturation=1.2
cut.filter doc=$tl clip=$b grayscale=true
cut.filter doc=$tl clip=$a lut=warm.cube
cut.info doc=$tl
cut.preview doc=$tl t=2
cut.render doc=$tl output=graded-a.png at=2 width=640
cut.render doc=$tl output=graded-b.png at=8 width=640
cut.filter doc=$tl clip=$a reset=true
cut.render doc=$tl output=ungraded-a.png at=2 width=640
undo
cut.render doc=$tl output=graded.mp4 quality=draft
```

```sh
$ wetwaresuite batch colour.txt
...
filtered 2 clips (timeline 10s)
...
cut1: 1920x1080 30fps, background black, 10s
v1 (video)
  c1 video interview.mp4 at 0-6s (6s) source 0-6 brightness=0.05 contrast=1.15 saturation=1.2 lut=warm.cube
  c2 video broll.mp4 at 6-10s (4s) source 0-4 brightness=0.05 contrast=1.15 saturation=1.2 grayscale
...
frame at 2s of cut1
t: 2
attachment: cut1-2s.png (52100 bytes)
...
undone
rendered graded.mp4
```

- Filters add up: a second `cut.filter` changes only what it names. `reset=true` clears a clip's filters first (and `undo` brings them back).
- `cut.preview` attaches a PNG, which Claude sees over MCP; from the command line it just reports the attachment, so render a `.png` to look yourself.
- LUTs are the `.cube` files colourists and cameras share (3D LUTs, applied with ffmpeg's `lut3d`).

**CLI.** A one-off grade of a single file, piped:

```sh
printf '%s\n' 'cut.new -> t' 'cut.add_clip doc=$t path=interview.mp4' 'cut.filter doc=$t lut=warm.cube' \
  'cut.render doc=$t output=interview-warm.mp4' | wetwaresuite batch
```

**Claude.**

> The b-roll looks washed out next to the interview. Make it punchier, then show me both.

Claude calls `cut_filter` on the b-roll clip (contrast and saturation up a little), `cut_preview` at a time in each clip, and tweaks until they match.

## Checking and fixing an edit

Reopen the short from task 1 and work on it. Each line is one undo step (batch scripts take `#` comments only on their own lines):

```text
# fix.txt
cut.open path=short.wwcut -> s
cut.info doc=$s
# one frame, attached as a PNG (width=640 by default); render a .png to get it as a file
cut.preview doc=$s t=12.5
cut.render doc=$s output=f.png at=12.5
# cut c2 in two at 8 s: the second half gets a new id (c5)
cut.split_clip doc=$s clip=c2 at=8
# play a different part of the source, or just make it shorter
cut.trim_clip doc=$s clip=c5 in=17 out=21
cut.trim_clip doc=$s clip=c5 duration=2
cut.move_clip doc=$s clip=c3 start=1 track=v3
# ripple closes the gap the clip leaves
cut.delete_clip doc=$s clip=c5 ripple=true
cut.set_clip doc=$s clip=c1 volume=0.5 fade_in=1
cut.set_clip doc=$s clip=c1 lut=warm.cube crop=0,0,1080,1080
# empty and zero values take things off again
cut.set_clip doc=$s clip=c1 lut="" crop=0,0,0,0
# silence all the music at once
cut.set_track doc=$s track=a1 muted=true
undo
redo
cut.info doc=$s
```

```sh
$ wetwaresuite batch fix.txt
opened cut1 (1080x1920 30fps, 3 tracks, 4 clips, 13.50s, short.wwcut)
...
frame at 12.5s of cut1
t: 12.5
attachment: cut1-12.5s.png (72025 bytes)
rendered f.png
...
split c2 at 8s into c2 and c5 (timeline 13.5s)
...
trimmed c5 to 4s (timeline 13.5s)
...
trimmed c5 to 2s (timeline 13.5s)
...
moved c3 to v3 at 1s (timeline 13.5s)
...
deleted c5 (timeline 13.5s)
...
a1 muted
track: a1
undone
redone
cut1: 1080x1920 30fps, background black, 13.5s
v1 (video)
  c1 video interview.mp4 at 0-7s (7s) source 2-9 volume=0.5 fade_in=1 fit=fill
  c2 video interview.mp4 at 6.5-8s (1.5s) source 15-16.5 fit=fill transition=crossfade 0.5s from c1
v2 (video): empty
v3 (video)
  c3 title "3 ways to edit faster" at 1-4s (3s) fade_in=0.3 fade_out=0.3 size=90 color=white position=top box=black@0.5
a1 (audio, muted)
  c4 audio music.mp3 at 0-13.5s (13.5s) source 0-13.5 volume=0.25 fade_in=1 fade_out=2
```

Every edit command also takes `preview=true`, which attaches the frame in the middle of the edited clip, so Claude sees the effect of each change without a separate call. Edits that would break a clip are refused and change nothing:

```text
cut.set_clip doc=cut1 clip=c1 fade_in=3 fade_out=3     -> fade_in + fade_out is longer than the clip (4s)
cut.add_clip doc=cut1 path=a.mp4 out=9                 -> out (9s) is past the end of the source, which is 3s long
cut.split_clip doc=cut1 clip=c1 at=4                   -> at must be inside the clip, between 1 and 3s
```

## Formats and quality

`cut.render` picks the format from the output's extension:

| Extension | What you get |
| --- | --- |
| `.mp4`, `.mov`, `.mkv`, `.m4v` | H.264 video (yuv420p) + AAC sound, `+faststart` for the web |
| `.webm` | VP9 video + Opus sound |
| `.gif` | animated GIF (palette per file), `gif_fps`, `gif_width`, no sound |
| `.mp3`, `.wav`, `.m4a`, `.aac`, `.flac`, `.ogg`, `.opus` | the mixed sound only |
| `.png`, `.jpg` | one frame at `at` seconds, `width` to scale it |

| `quality` | H.264 | VP9 | sound |
| --- | --- | --- | --- |
| `draft` | ultrafast, CRF 28 | CRF 42, realtime | 128 kb/s |
| `good` (default) | veryfast, CRF 22 | CRF 33 | 192 kb/s |
| `best` | slow, CRF 18 | CRF 24, slower | 256 kb/s |

`mode=auto` (default) stream-copies when it can; `mode=copy` fails with the reason when it can't (`can't stream-copy: it has more than one track with clips (use mode=encode or auto)`), `mode=encode` always re-encodes. Stream copy needs: one track, only video clips, all from sources with the same codecs, size and frame rate as the timeline, back to back from 0 s, at normal speed and volume, with no fades, boxes, crops, filters, transitions or titles, and an output of the same kind as the sources (`.mp4`/`.mov`/`.m4v` count as one kind). Its cuts land on keyframes.

## Command reference

### Timelines

| Command | What it does | Main parameters |
| --- | --- | --- |
| `cut.new` | new timeline, returns `cut1` | `width` (1920), `height` (1080), `fps` (30), `background` (black) |
| `cut.open` | open a `.wwcut` | `path` |
| `cut.save` | save as `.wwcut` | `doc`, `path` |
| `cut.close` | close (undo reopens) | `doc` |
| `cut.info` | describe tracks and clips | `doc` |
| `cut.settings` | change size, fps, background | `doc`, `width`, `height`, `fps`, `background` |
| `cut.set_track` | mute a track or hide its picture | `doc`, `track`, `muted`, `hidden` |
| `cut.probe` | what ffprobe says about a file | `path` |

### Adding and editing clips

| Command | What it does | Main parameters |
| --- | --- | --- |
| `cut.add_clip` | a video file | `doc`, `path`, `track` (v1), `start`, `in`, `out`, `duration`, `speed`, `volume`, `mute`, picture properties, `preview` |
| `cut.add_image` | a still | `doc`, `path`, `duration` (5), `track`, `start`, picture properties |
| `cut.add_title` | text over the video | `doc`, `text`, `start` (0), `duration` (3), `position`, `font_size`, `color`, `font`, `box`, `box_color`, `x`, `y`, `fade_in`, `fade_out`, `opacity` |
| `cut.add_audio` | music, voice-over, effects | `doc`, `path`, `track` (a1), `start`, `in`, `out`, `duration`, `volume`, `fade_in`, `fade_out`, `speed` |
| `cut.move_clip` | new start and/or track | `doc`, `clip`, `start`, `track` |
| `cut.trim_clip` | change the source range | `doc`, `clip`, `in`, `out`, `duration` |
| `cut.split_clip` | cut in two at a time | `doc`, `clip`, `at` |
| `cut.delete_clip` | remove | `doc`, `clip`, `ripple` |
| `cut.set_clip` | change any property | `doc`, `clip`, any property below |
| `cut.transition` | blend with the clip before | `doc`, `clip`, `type` (crossfade), `duration` (1) |
| `cut.filter` | colour | `doc`, `clip` (all if left out), `brightness`, `contrast`, `saturation`, `gamma`, `grayscale`, `lut`, `reset` |
| `cut.preview` | one frame as an attached PNG | `doc`, `t`, `width` (640) |
| `cut.render` | export | `doc`, `output`, `quality`, `mode`, `at`, `width`, `subtitles`, `gif_fps`, `gif_width`, `overwrite` |

### Clip properties

| Property | Applies to | Meaning |
| --- | --- | --- |
| `start` | all | timeline time where the clip begins (s) |
| `in`, `out` | video, audio | source range (s) |
| `duration` | all | timeline length (s); sets `out = in + duration x speed` |
| `speed` | video, audio | 0.05 to 20; sound keeps its pitch |
| `volume`, `mute` | video, audio | 1 = as recorded, 0.3 = quiet, up to 10 |
| `fade_in`, `fade_out` | all | seconds; pictures fade from/to transparent, sound from/to silence |
| `opacity` | pictures, titles | 0 to 1 |
| `x`, `y`, `width`, `height` | pictures, titles (`x`,`y`) | the picture box in pixels; `width` 0 = full frame |
| `fit` | pictures | `fit` (letterbox), `fill` (crop to fill), `stretch` |
| `crop` | pictures | `x,y,w,h` in source pixels; `0,0,0,0` removes it |
| `brightness` | pictures | -1 to 1 (0) |
| `contrast` | pictures | 0 to 3 (1) |
| `saturation` | pictures | 0 to 3 (1) |
| `gamma` | pictures | 0.1 to 10 (1) |
| `grayscale` | pictures | true for black and white |
| `lut` | pictures | a `.cube` file, empty to remove |
| `text`, `font_size`, `color`, `font` | titles | `color` is a name, `#rrggbb` or `white@0.8`; `font` is a `.ttf`/`.otf` file or, if ffmpeg has fontconfig, a family name |
| `position` | titles | `center`, `top`, `bottom`, `lower-third`, `top-left`, `top-right`, `bottom-left`, `bottom-right`, `custom` (uses `x`,`y`; giving `x` or `y` sets it) |
| `box`, `box_color` | titles | a box behind the text, default `black@0.5` |

### One-shot file commands

| Command | What it does | Parameters |
| --- | --- | --- |
| `cut.trim_file` | cut a piece out | `input`, `output`, `start`, `end` or `duration`, `copy` |
| `cut.concat_files` | join files end to end | `inputs` (comma-separated), `output`, `mode`, `quality` |
| `cut.extract_audio` | save the sound | `input`, `output` (.mp3 .wav .flac .m4a .aac .ogg .opus .mka) |
| `cut.mute` | remove the sound | `input`, `output` |
| `cut.speed` | speed up / slow down | `input`, `output`, `factor`, `quality` |
| `cut.thumbnails` | contact sheet | `input`, `output`, `columns` (4), `rows` (3), `width` (320), `preview` |
| `cut.gif` | GIF of a range | `input`, `output`, `start`, `duration` (3), `fps` (12), `width` (480) |
| `cut.subtitles` | burn in .srt/.ass | `input`, `subtitles`, `output`, `font_size`, `color`, `quality` |

All of them take `overwrite=true` to replace an existing output. Run `wetwaresuite help <command>` for every parameter with its default.

## Limitations

- `cut.play` plays the edit's sound only (mixed by ffmpeg first, see [playback](playback.md)); there's no live video playback in the window yet, so check pictures with `cut.preview`.
- Rendering is as fast as ffmpeg encoding the whole edit; there's no background rendering or proxy media. Use `quality=draft` while you iterate.
- Titles are one line of plain text each (no rich text, outlines or animation besides fades); drawn with ffmpeg's `drawtext`. When ffmpeg has no fontconfig, a system font file is found and passed in (DejaVu, Liberation, Noto, Arial, Helvetica, ...); set `font=/path/to/font.ttf` to pick one.
- Transitions only join clips on the same track, and every clip in a chain of transitions is drawn in the first clip's box. `fadeblack` also darkens the letterbox bars around a picture that doesn't fill the frame.
- Sound is mixed at the levels you set; there's no automatic ducking, normalising or noise reduction (the audio tool will do that).
- No keyframed animation: a clip's box, opacity and colour are the same for its whole length (split it to change them part-way).
- Stream-copy cuts (`mode=copy`, `cut.trim_file copy=true`, `cut.concat_files` of identical files) start on keyframes, so they can begin a little before the time you asked for.
- `.wwcut` files store absolute paths to the media; moving the media breaks the project (edit the paths in the JSON, or relative paths are read relative to the `.wwcut` file).
- Frame rates are constant: variable-frame-rate phone footage is converted to the timeline's rate.
