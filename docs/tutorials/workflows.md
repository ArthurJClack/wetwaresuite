# Workflows: moving work between apps

Each wetwaresuite app is its own program (see [apps](apps.md)), but they are made to work together. You paint a texture in **image**, put it on a model in **model**, and ship the model in **game**; you design a part in **cad**, make it organic in **model**, and send it back to **cad** to print. This page shows how, step by step, for people using the command line, for scripts, and for Claude.

Everything here works offline, with no AI connected.

## How a handoff works

Three commands, in every app:

| command | what it does |
| --- | --- |
| `app.send doc=<id> to=<app>` | saves the open document in the format the other app wants, into the **handoff folder**, and tells you the exact command to run next |
| `app.receive` | in the other app: picks up the newest file sent to it and opens it with the right command |
| `app.inbox` | lists what's waiting (`app=all` for every app, `all=true` to include ones already picked up) |

Two more help you find your way:

| command | what it does |
| --- | --- |
| `app.open_with path=<file>` | says which app opens a file and the command to open it there |
| `app.installed` | lists which apps you have and where they are |
| `app.targets doc=<id>` | lists the apps an open document can be sent to, and which are installed (the window's Send to... list) |
| `app.launch app=<app> [path=<file>]` | starts another installed app in its own window, optionally opening a file |

The handoff folder is in your personal wetwaresuite folder:

| system | handoff folder |
| --- | --- |
| Linux | `~/.local/share/wetwaresuite/handoff` |
| macOS | `~/Library/Application Support/wetwaresuite/handoff` |
| Windows | `%APPDATA%\wetwaresuite\handoff` |

It holds the sent files (ordinary PNG, STL, glTF, WAV... that any program can open) and a small `manifest.json` that records who sent what to whom. You can open the files yourself with anything; the manifest is only there so `app.receive` knows what's new.

### What format each handoff uses

`app.send` picks the best format for the trip. You don't have to remember these, but here they are:

| from | to | as | then in the target |
| --- | --- | --- | --- |
| image (picture) | model | PNG texture | `render.add ... texture=FILE`, `render.material ... texture=FILE`, or `sculpt.displace image=FILE` |
| image (picture) | game | PNG texture | `forge.seamless input=FILE`, `forge.normal_map input=FILE` |
| image (picture) | office / video / draw | PNG | `layout.place_image`, `cut.add_image` |
| draw (vector) | office | SVG | `layout.place_image doc=<layout> path=FILE` |
| draw (vector, board) | image | PNG | opens in photo |
| draw (font) | office | TTF | `layout.font_add doc=<layout> path=FILE` |
| model (sculpt) | game | glTF (.glb) | opens as a game mesh (`forge.mesh_open`) |
| model (sculpt) | cad | STL | loads on the printer bed (`slicer.load`) |
| model (scene) | image / office / video | rendered PNG | opens in photo, or place it |
| model (animation) | game | sprite sheet PNG | `forge.sprite_split input=FILE` |
| model (animation) | video | GIF | `cut.add_clip doc=<cut> path=FILE` |
| game (mesh) | model | glTF (.glb) | opens for sculpting |
| game (mesh) | cad | STL | loads on the printer bed |
| cad (part) | model | STL | opens for sculpting |
| cad (part) | game | glTF (.glb) | opens as a game mesh |
| cad (blueprint) | draw / office | SVG | opens as vector art, or place it |
| electronics (circuit) | office / draw | SVG | place it, or open as vector art |
| sound (audio, song) | video | WAV | `cut.add_audio doc=<cut> path=FILE` |
| office (layout) | office | PDF | the print-ready file |
| maps (map) | image / office | PNG / SVG | opens in photo, or place it |

Any file can also be sent as it is: `app.send path=photo.jpg to=image`.

## With the windows (GUI)

Every app opens a window when you double-click it, and the window has the same handoff built in.
Here is walkthrough 1 (below) with the mouse instead of typing:

1. **Start image** by double-clicking `wetwaresuite-image`.
2. Press **Start something new**, pick **New canvas (Paint)**, set the size (256 by 256) and press
   **Run**. Paint your texture with the **Brush** above the picture.
3. Press **Send to...** next to the picture's name and click **Model**. The right side says
   **Sent to Model**: image saved your picture as a PNG texture in the handoff folder.
4. Press **Open Model now** under that message (or double-click `wetwaresuite-model` yourself).
5. In Model's window the home screen shows *A texture from Image is waiting: crate.png.* Press
   **Open it**. Because a texture isn't something you open on its own, the right side tells you
   what to do with it, for example which action puts it on a model's material. Search for that
   action (Ctrl+K); the result also shows where the texture's file is, so you can pick it with the
   form's **Browse...** button.
6. To go on to **game**, open or make your model, press **Send to...** again and pick **Game**.

A few things to know:

- **Send to...** only lists the apps that can use the kind of file you have open. Apps you haven't
  installed are greyed out with *not installed*.
- The inbox banner checks again every time you switch to a window, so you can keep two apps open
  side by side and send work back and forth. **Not now** hides it for a while; nothing is lost.
- If you open or drop a file that belongs to another app (a `.wav` in image, an `.stl` in model), a
  banner offers **Open in Sound** (or whichever app it is) and starts that app with the file.
- In the all-in-one `wetwaresuite` window every app is there already: **Send to...** converts the
  file and opens the result right away in the same window.

The buttons run exactly the commands below (`app.targets`, `app.send`, `app.inbox`,
`app.receive`, `app.open_with`, `app.launch`), so a window, a script and Claude can pass work to
each other: something Claude sends from `wetwaresuite-image mcp` shows up in the model window's
banner, and a file you send from a window shows up in `wetwaresuite-model app.inbox`.

To start another app from the command line (it opens in its own window and keeps running on its own):

```sh
wetwaresuite-image app.launch app=model                  # just start it
wetwaresuite-image app.launch app=model path=head.obj    # and open a file there
```

## Walkthrough 1: a crate texture, on a model, into a game

You need the **image**, **model** and **game** apps (`hub install image model game --from <folder>`).

**1. Paint the texture in image.** Save this as `texture.wws`:

```
paint.new width=256 height=256 background="#8a5a2b" -> crate
paint.shape doc=$crate kind=rect x=8 y=8 width=240 height=240 color="#5a3a1a" filled=false line_width=12
paint.stroke doc=$crate points=16,16,240,240 color="#5a3a1a" size=14
app.send doc=$crate to=model name=crate
```

and run it:

```sh
wetwaresuite-image batch texture.wws
```

The last lines tell you where the texture went and what to do next:

```
sent to model (texture): /home/you/.local/share/wetwaresuite/handoff/crate.png
next: wetwaresuite-model app.receive
then: use it as a texture: render.material doc=<scene> name=<material> texture=/home/you/.local/share/wetwaresuite/handoff/crate.png, ...
```

**2. Use it in model.** Check it arrived:

```sh
wetwaresuite-model app.inbox
```

Now make a box with the texture, render a picture of it, and make a game-ready mesh embossed with the same pattern. Save as `model.wws` (put your own handoff folder in the paths):

```
render.new -> scene
render.add doc=$scene type=box size=1,1,1 texture="/home/you/.local/share/wetwaresuite/handoff/crate.png" name=crate
render.render doc=$scene output=crate-render.png width=320 height=240 samples=16
sculpt.new shape=cube -> box
sculpt.displace doc=$box image="/home/you/.local/share/wetwaresuite/handoff/crate.png" amount=0.02 mapping=planar
app.send doc=$box to=game name=crate
```

```sh
wetwaresuite-model batch model.wws
```

You get `crate-render.png` (a picture of the textured box) and the model is sent to game as `crate.glb`.

**3. Finish it in game.** `app.receive` opens the mesh; capture its id and make levels of detail:

```
app.receive -> mesh
forge.mesh_lods doc=$mesh output=crate.glb ratios=1,0.5,0.25
```

```sh
wetwaresuite-game batch game.wws
wetwaresuite-game forge.seamless input=/home/you/.local/share/wetwaresuite/handoff/crate.png output=crate-tile.png
```

You now have `crate_lod0.glb`, `crate_lod1.glb`, `crate_lod2.glb` and a tileable `crate-tile.png` for your game engine.

## Walkthrough 2: a cad part made organic, then printed

You need **cad** and **model**.

```sh
printf 'cad.new -> part\ncad.box doc=$part width=30 depth=30 height=5\napp.send doc=$part to=model name=plate\n' | wetwaresuite-cad batch
wetwaresuite-model app.receive          # opens plate.stl for sculpting as sculpt1
```

Sculpt it (see [sculpt](sculpt.md)), then send it back to print. In one script:

```
app.receive -> plate
sculpt.subdivide doc=$plate
app.send doc=$plate to=cad name=plate-organic
```

Back in cad, `app.receive` puts the STL on the printer bed, and you write G-code:

```
app.receive -> job
slicer.export_gcode doc=$job path=plate.gcode
```

## Walkthrough 3: music for a video

You need **sound** and **video**.

```sh
printf 'beats.generate genre=lofi bars=4 -> song\napp.send doc=$song to=video name=theme\n' | wetwaresuite-sound batch
```

The song is rendered to `theme.wav` in the handoff folder. In video:

```
cut.new -> edit
cut.add_clip doc=$edit path=holiday.mp4
cut.add_audio doc=$edit path="/home/you/.local/share/wetwaresuite/handoff/theme.wav"
cut.render doc=$edit path=holiday-with-music.mp4
```

(Reading and writing video files needs ffmpeg installed; see [cut](cut.md).)

## Walkthrough 4: a logo on a poster

You need **draw** and **office**.

```sh
printf 'vector.new width=200 height=100 -> logo\nvector.add_rect doc=$logo x=10 y=10 width=180 height=80 fill=teal\napp.send doc=$logo to=office name=logo\n' | wetwaresuite-draw batch
```

In office, `app.receive` returns the SVG's path, ready to place:

```
layout.new preset=poster_a3 -> poster
app.receive -> logo
layout.place_image doc=$poster path=$logo x=20 y=20 w=120 h=60
layout.export doc=$poster path=poster.pdf
```

## Which app opens this file?

```sh
wetwaresuite-files app.open_with path=part.stl
```

```
part.stl opens in cad (precise parts, technical drawings and floor plans, 3D printing)
next: wetwaresuite-cad run slicer.load path=/home/you/part.stl
```

With `open=true`, an app opens the file right away if it's one of its own.

## In the all-in-one binary

`wetwaresuite` has every app inside, so `app.send` opens the result in the target app's tools straight away in the same session, and you keep working with the new document id. The file is still written to the handoff folder, so separate apps see it too.

```
paint.new width=64 height=64 -> art
app.send doc=$art to=model -> tex      # tex is the texture file
```

## For Claude (optional)

Connect the apps you use (`claude mcp add wetwaresuite-image -- wetwaresuite-image mcp`, and the same for model and game), or the all-in-one `wetwaresuite mcp`. Each app tells Claude which other apps are installed and how to hand off, so you can simply ask:

> Paint a 256x256 wooden crate texture in image, then use it on a box in model and render a picture, then send the box to game and make LODs.

Claude calls `app_send` in the image app, `app_receive` in model, and so on, exactly like the scripts above. If an app isn't installed, Claude is told so and can say which one to install.

## Good to know

- Handoff files are never deleted automatically. Clear the folder whenever you like.
- `app.send` never overwrites: a second `crate.png` becomes `crate-2.png`.
- `app.receive id=<id>` picks a specific handoff from `app.inbox`; `open=false` just returns the file.
- Set `WETWARESUITE_HOME` to use a different wetwaresuite folder (for example one per project).
