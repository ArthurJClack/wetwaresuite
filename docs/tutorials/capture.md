# Capture: screenshots and screen recordings

`capture` takes screenshots (the whole desktop, one monitor or a region, optionally after a delay), records the screen to MP4, WebM or GIF with or without a microphone, and takes webcam pictures. It also lets Claude look at your screen: a screenshot without an output file comes back as an image Claude can see.

```sh
wetwaresuite capture.screenshot output=region.png x=100 y=50 width=400 height=300
```

There is no GUI yet; the GUI that comes later will call these same commands. Each task below shows the CLI way, the batch-script way and the "ask Claude" way. Every CLI and batch example here was run against the real binary, on Linux with a virtual 800x600 screen (that's why the sizes below are small; your desktop gives your screen's size).

## What you need

Capturing goes through [ffmpeg](https://ffmpeg.org)'s screen and camera devices, so ffmpeg must be installed and on your `PATH` (Debian/Ubuntu `sudo apt install ffmpeg`, macOS `brew install ffmpeg`, Windows `winget install ffmpeg`). On Wayland, screenshots can also use [grim](https://sr.ht/~emersion/grim/) (`sudo apt install grim`).

How each system is captured (`backend=auto` picks it):

| System | Screenshots | Recording | Sound | Webcam |
| --- | --- | --- | --- | --- |
| Linux, X11 | ffmpeg `x11grab` on `$DISPLAY` | `x11grab` | PulseAudio/PipeWire (`pulse`) | `v4l2` (`/dev/video0`) |
| Linux, Wayland | `grim` if installed, else `x11grab` through XWayland | `x11grab` through XWayland | `pulse` | `v4l2` |
| Windows | `gdigrab` (all monitors), or `backend=ddagrab` (Desktop Duplication, one monitor, faster) | same | DirectShow (`dshow`) | DirectShow |
| macOS | `avfoundation` ("Capture screen N") | same | avfoundation | avfoundation |

On macOS, the first capture asks for **Screen Recording** permission (and Microphone/Camera for those) for the terminal or app that runs `wetwaresuite`; grant it in System Settings > Privacy & Security and run the command again.

## Where am I? List displays and devices

```sh
$ wetwaresuite capture.list_displays
platform: Linux
ffmpeg: installed
DISPLAY: :91
WAYLAND_DISPLAY: not set
grim: not installed
screenshot method: x11grab
monitors:
  0: screen 800x600 at 0,0 (primary)
platform: Linux
backend: x11grab
monitors: 1
width: 800
height: 600
```

- Monitors are numbered from 0; pass the number as `monitor=`. On Linux the list comes from `xrandr --listmonitors` (install `x11-xserver-utils` for multi-monitor details); without it the whole X screen is measured and shown as monitor 0. On Windows each monitor shows its position on the virtual desktop (monitors left of the main one have negative x). On macOS the screens are the avfoundation "Capture screen" devices.
- Region coordinates are pixels from the top-left of the monitor you name (or of the whole desktop without `monitor`).

```sh
$ wetwaresuite capture.list_devices
cameras: none found
sound inputs:
  default
cameras: 0
audio_inputs: 1
```

On Linux, cameras are the `/dev/video*` devices and sound inputs are `default` plus every PulseAudio/PipeWire source (`pactl list short sources`). On Windows and macOS the list is what ffmpeg's DirectShow or avfoundation reports; macOS devices are used by their number.

## Task 1: a screenshot of a region

**CLI.**

```sh
$ wetwaresuite capture.screenshot output=region.png x=100 y=50 width=400 height=300
saved a screenshot to region.png
region.png
width: 400
height: 300
backend: x11grab
output: region.png
```

More ways to aim it:

```sh
$ wetwaresuite capture.screenshot output=desktop.png
saved a screenshot to desktop.png
...
width: 800
height: 600
$ wetwaresuite capture.screenshot monitor=0 output=monitor0.png
saved a screenshot to monitor0.png
...
$ wetwaresuite capture.screenshot output=menu.jpg delay=3 cursor=true
saved a screenshot to menu.jpg
...
```

- `delay=3` waits three seconds first, so you can open a menu or hover over something. `cursor=true` includes the mouse pointer (left out by default).
- The extension picks the format: `.png` (lossless, best for text) or `.jpg` (smaller, for photos and video frames).
- A region that leaves the monitor, or half a region, is refused with the reason (`the region goes outside monitor 1 (1280x1024)`, `a region needs both width and height`). So is a monitor that doesn't exist: `there's no monitor 2 (capture.list_displays shows the screens)`.
- An existing file is replaced (pass `overwrite=false` to refuse instead).

**Batch.** The same corner of the screen three times, five seconds apart (a quick way to document steps of a slow process):

```text
# shots.txt: the same corner of the screen three times, five seconds apart
capture.screenshot output=step1.png x=0 y=0 width=400 height=300
capture.screenshot output=step2.png x=0 y=0 width=400 height=300 delay=5
capture.screenshot output=step3.png x=0 y=0 width=400 height=300 delay=5
```

```sh
$ wetwaresuite batch shots.txt
saved a screenshot to step1.png
step1.png
width: 400
height: 300
backend: x11grab
output: step1.png
saved a screenshot to step2.png
...
saved a screenshot to step3.png
...
```

Screenshots are ordinary image files, so the photo tool can work on them in the same script, e.g. `photo.open path=step1.png -> s` then `photo.resize doc=$s width=200` and `photo.save doc=$s path=step1-small.png`.

**Claude.**

> Take a screenshot of the top-left 400x300 of my screen and save it as region.png.

Claude calls `capture_screenshot` with `output`, `x`, `y`, `width` and `height`. To check what it captured it can pass `preview=true` and look at the attached image.

## Task 2: record 30 seconds of the screen with the microphone

**CLI.**

```sh
$ wetwaresuite capture.record output=demo.mp4 duration=30 audio=true
```

On a desktop this records the whole screen and the default microphone for 30 seconds into an H.264 + AAC MP4 and returns when it's done:

```text
recorded 30s of the screen to demo.mp4 with sound
output: demo.mp4
...
```

The machine these examples ran on has no sound server, which shows what happens then: nothing is half-written, and the message says what to check:

```sh
$ wetwaresuite capture.record output=demo-mic.mp4 duration=30 audio=true
ffmpeg failed (exit 253): [in#1 @ 0x5632b38c0640] Error opening input: No such process
Error opening input file default.
Error opening input files: No such process
(the sound input couldn't be opened: is PulseAudio/PipeWire running? capture.list_devices lists inputs; audio=false records without sound)
```

Without sound:

```sh
$ wetwaresuite capture.record output=demo.mp4 duration=30
recorded 30s of the screen to demo.mp4
demo.mp4
output: demo.mp4
bytes: 37685
duration: 30
backend: x11grab
```

- Pick a microphone with `audio_device=`: a PulseAudio source name on Linux (from `capture.list_devices`, e.g. `alsa_input.usb-Blue_Yeti-00.analog-stereo`), the device name on Windows (required there; the first microphone is used if you leave it out), or its number on macOS (default `0`).
- `fps` (default 30) and `quality=draft|good|best` (default good) trade size and CPU for smoothness. `draft` uses x264's ultrafast preset, which keeps up on slow machines.
- `monitor`, `x`, `y`, `width`, `height` record part of the screen, like screenshots. Odd sizes are rounded down to even ones (video encoders need it).
- `.webm` records VP9 + Opus, `.mkv` and `.mov` work too, and `.gif` makes an animated GIF (no sound, at most 15 frames per second):

```sh
$ wetwaresuite capture.record output=clip.gif duration=4 fps=10 x=0 y=0 width=640 height=360
recorded 4s of the screen to clip.gif
...
```

**Batch.** Record, then cut the recording down with the [cut](cut.md) tool in the same script:

```text
# demo.txt: record, then cut the recording down with the cut tool
capture.record output=raw.mp4 duration=10 fps=30
cut.trim_file input=raw.mp4 output=demo-short.mp4 start=2 end=8
cut.gif input=raw.mp4 output=demo.gif start=2 duration=3 width=400
cut.thumbnails input=raw.mp4 output=demo-sheet.jpg columns=4 rows=2 width=200
```

```sh
$ wetwaresuite batch demo.txt
recorded 10s of the screen to raw.mp4
...
trimmed 2-8s of raw.mp4 into demo-short.mp4
...
duration: 6
made demo.gif from 2-5s of raw.mp4
...
contact sheet of raw.mp4: 4x2 frames in demo-sheet.jpg
...
```

For a full tutorial video, put the recording on a cut timeline with your webcam in the corner (see [picture-in-picture](cut.md#task-2-picture-in-picture)).

**Claude.**

> Record my screen for 30 seconds with the microphone and save it to ~/Videos/bug-report.mp4.

Claude calls `capture_record` with `duration=30` and `audio=true`. The call returns when the recording is finished, so tell Claude before it starts what you're going to show. If the microphone fails it reads the hint, calls `capture_list_devices` and retries with the right `audio_device`, or without sound if you agree.

## Task 3: let Claude see your screen

When `capture.screenshot` has no `output`, nothing is saved: the screenshot comes back as an image attached to the result. Over MCP, that's an image Claude looks at, which is how it can check a window, read an error dialog or follow along while it drives the other tools.

**Claude.**

> Take a screenshot and tell me what the error dialog says.

> Look at my second monitor and check whether the render finished.

Claude calls `capture_screenshot` (with `monitor=1` for the second monitor, or a region to zoom in on something small) and reads the picture. This is the exchange under the hood (the image data shortened):

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"capture_screenshot","arguments":{"max_size":800}}}
{"id":2,"jsonrpc":"2.0","result":{"content":[{"text":"took a screenshot\nwidth: 800\nheight: 600\nbackend: x11grab\nattachment: screenshot.png (19598 bytes)","type":"text"},{"data":"iVBORw0KGgoAAAANSUhEUgAAAyAAAAJY...","mimeType":"image/png","type":"image"}],"isError":false,...}}
```

- `max_size` (default 1280) caps the longest side of the attached image; the screen is scaled down to fit, so a 4K screen arrives as 1280x720. For small text, take a region instead of the whole screen: `x`, `y`, `width`, `height` are in real screen pixels and keep full detail.
- `preview=true` also attaches the image when you do save to a file.
- Claude only sees the screen when it calls the tool; nothing is captured in the background.

**CLI.** From the command line the attachment is only reported (`attachment: screenshot.png (5154 bytes)`), so give an `output` to keep the picture:

```sh
$ wetwaresuite capture.screenshot max_size=400
took a screenshot
width: 800
height: 600
backend: x11grab
attachment: screenshot.png (5154 bytes)
```

**Batch.** Same as the CLI; in scripts, save to files.

## Task 4: a webcam picture

**CLI.**

```sh
$ wetwaresuite capture.webcam output=me.jpg
```

- `device` picks the camera (`/dev/video2` on Linux, a name on Windows, a number on macOS); `width` and `height` ask the camera for a size it supports, e.g. `width=1280 height=720`.
- `warmup` (default 0.5 s) lets the camera settle its exposure before the picture is taken; raise it if pictures come out dark.
- Without a camera it says so:

```sh
$ wetwaresuite capture.webcam output=me.jpg
no webcam found (/dev/video0 doesn't exist; capture.list_devices lists cameras)
```

**Batch.** (The machine these examples ran on has no camera, so this one is shown without its output.)

```text
capture.webcam output=before.jpg warmup=1
capture.webcam output=after.jpg warmup=1 width=1280 height=720
```

**Claude.**

> Take a picture with my webcam and tell me if the lighting is good enough for a video call.

Claude calls `capture_webcam` with `preview=true` and looks at the picture.

## Headless machines and containers

On a server, in a container or over SSH there's no screen. Capture commands fail straight away with an explanation instead of hanging:

```sh
$ wetwaresuite capture.screenshot output=x.png
there's no screen to capture here: DISPLAY and WAYLAND_DISPLAY aren't set, so this looks like a server, container or SSH session. Run it from a terminal inside your desktop session, or give a headless machine a virtual screen: `Xvfb :99 -screen 0 1920x1080x24 &` and pass display=:99
```

To capture a program running on a virtual screen (e.g. to make screenshots of a GUI in CI), start Xvfb and pass `display=` (or set `DISPLAY`):

```sh
Xvfb :91 -screen 0 800x600x24 &
DISPLAY=:91 my-app &
$ wetwaresuite capture.screenshot display=:91 output=virtual.png
saved a screenshot to virtual.png
virtual.png
width: 800
height: 600
backend: x11grab
output: virtual.png
```

Over SSH to a desktop machine, `display=:0` captures the screen of the person logged in there (if your user may open it).

## Command reference

### capture.screenshot

| Parameter | Default | Meaning |
| --- | --- | --- |
| `output` | none | `.png` or `.jpg` to write; leave out to only attach the image |
| `monitor` | all | which monitor (0, 1, ...) |
| `x`, `y`, `width`, `height` | whole screen | region in pixels, relative to the monitor when one is given |
| `delay` | 0 | seconds to wait first |
| `cursor` | false | include the mouse pointer |
| `backend` | auto | `x11grab`, `grim`, `gdigrab`, `ddagrab`, `avfoundation` |
| `display` | `$DISPLAY` | Linux: the X display, e.g. `:0` or `:99` |
| `preview` | false | attach the image even when saving (always on without `output`) |
| `max_size` | 1280 | longest side of the attached image |
| `overwrite` | true | replace an existing output |

### capture.record

| Parameter | Default | Meaning |
| --- | --- | --- |
| `output` | required | `.mp4`, `.mkv`, `.mov`, `.webm` or `.gif` |
| `duration` | 10 | seconds (up to 4 hours) |
| `fps` | 30 | frames per second (1..120; GIFs use at most 15) |
| `monitor`, `x`, `y`, `width`, `height`, `display` | | as for screenshots |
| `audio` | false | record sound too (not for GIFs) |
| `audio_device` | default | PulseAudio source (Linux), device name (Windows), number (macOS) |
| `cursor` | true | include the mouse pointer |
| `quality` | good | `draft` (fast, bigger), `good`, `best` (slower) |
| `backend` | auto | `x11grab`, `gdigrab`, `ddagrab`, `avfoundation` |
| `overwrite` | true | replace an existing output |

### capture.webcam

| Parameter | Default | Meaning |
| --- | --- | --- |
| `output` | required | `.png` or `.jpg` |
| `device` | first camera | `/dev/videoN`, a DirectShow name, an avfoundation number |
| `width`, `height` | camera default | ask for this size |
| `warmup` | 0.5 | seconds to let the camera adjust first |
| `preview`, `max_size` | false, 1280 | attach the picture |

### capture.list_displays, capture.list_devices

No parameters (`list_displays` takes `display`). They change nothing and are safe to call any time.

## Limitations

- Recording blocks until it's done; there's no start/stop recording of unknown length yet (pick a `duration`, then trim with `cut.trim_file`).
- Wayland: screenshots need `grim` (wlroots compositors such as Sway; GNOME and KDE don't support it) or XWayland, which only sees X11 windows. Recording on pure Wayland isn't supported; use the desktop's own recorder or `wf-recorder`.
- Recording sound captures one input (a microphone or a monitor source). Recording what the computer plays at the same time as the microphone needs a mixed source set up in PulseAudio/PipeWire (or a loopback device on Windows and macOS).
- On X11 the monitor layout needs `xrandr`; without it only the whole screen is known.
- Window capture by name isn't supported; use a region.
- `ddagrab` (Windows) needs ffmpeg 6.0 or newer and captures one monitor.
- High-DPI screens are captured in real pixels: on a Retina Mac or a 200 % Windows display, regions are in physical pixels, not the "points" apps use.
