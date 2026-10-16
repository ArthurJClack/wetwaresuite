# Animate: 2D and 3D animation

`animate` makes animations without a screen. An animation is a canvas with a frame rate and a length in frames, holding a stack of **layers**. 2D layers hold shapes, text, pictures, hand-drawn **cels** and **bones**; 3D layers hold meshes and a camera. Any number or colour property can be **keyframed** with an easing curve, and elements can be **parented** to each other, which is how cutout rigs work. You check your work with previews (one frame, or a strip of several frames in one picture) and export to a PNG sequence, a sprite sheet with a JSON atlas for game engines, an animated GIF, or mp4/webm/mov video.

```text
animate.new width=320 height=240 frames=24 -> a
animate.element_add doc=$a kind=ellipse width=50 height=50 x=160 y=60 name=ball
animate.element_set doc=$a element=ball y=200 frame=12 ease=ease-in
animate.export_gif doc=$a output=drop.gif
```

There is no GUI yet; the GUI that comes later will call these same commands. Each task below shows the CLI way, the batch-script way and the "ask Claude" way. Every CLI and batch example here was run against the real binary (outputs are copied from those runs; long `key: value` lists are shortened with `...`).

## What you need

Nothing for drawing, previews, PNG sequences, sprite sheets and GIFs: they are all rendered and encoded inside `wetwaresuite` (the GIF encoder is built in). Video export (`animate.export_video`, mp4/webm/mov/mkv) hands the frames to [ffmpeg](https://ffmpeg.org), which must be on your `PATH` (`sudo apt install ffmpeg`, `brew install ffmpeg`, `winget install ffmpeg`). Without it, that one command says so and suggests the GIF or PNG export instead.

## How it works

- **An animation** is a document with an id like `anim1`. It has a canvas (`width` x `height` px, default 640x360), `fps` (default 24), a length in `frames` (default 48) and a `background` colour (`transparent` for sprites). **Frames count from 0**, so a 48-frame animation runs from frame 0 to frame 47. `animate.new` makes one 2D layer, `L1`, so you can start adding things at once.
- **Layers** (`L1`, `L2`, ...) stack bottom to top. Each is `2d` or `3d` and has a name, visibility, opacity and a blend mode (normal, multiply, screen, overlay, add, darken, lighten, difference). Anything that takes a layer id also takes its name.
- **Elements** (`e1`, `e2`, ..., one counter per animation) live on layers, bottom first. Kinds:

  | kind | layer | what it is |
  | --- | --- | --- |
  | `rect`, `ellipse` | 2d | a filled box or oval, `width` x `height`, optional outline (`stroke`, `stroke_width`) |
  | `polygon` | 2d | a filled shape through `points=x1,y1,x2,y2,...` |
  | `line` | 2d | a stroke through `points`, `stroke_width` wide |
  | `text` | 2d | one or more lines (`\n`) in sans, serif or mono, bold/italic, at `font_size` px |
  | `image` | 2d | a picture file (`source`), at its natural size or `width`/`height` |
  | `cel` | 2d | frame-by-frame drawings: draw on any frame, each drawing holds until the next |
  | `bone` | 2d | an invisible joint with a `length`, for rigs (shown with `bones=true` in previews) |
  | `mesh` | 3d | a primitive (`box`, `sphere`, `cylinder`, `cone`, `torus`, `plane`) or a mesh file (STL, OBJ, PLY, OFF, glTF/GLB, 3MF) |
  | `camera` | 3d | where the 3D layer is seen from: position, a `target` point and a `fov` |

  Elements are found by id or by `name`, so `element=ball` works once you named it.
- **2D coordinates** are canvas pixels from the top-left, `y` grows downwards, and `rotation` is degrees clockwise. `x`,`y` say where the element's **anchor** sits; `anchor_x`/`anchor_y` pick the anchor as a fraction of the element's box (0 = left/top, 0.5 = centre, the default, 1 = right/bottom). Rotation and scale turn around the anchor. Polygons and lines have no anchor: their points are relative to `x`,`y`. A cel's anchor is its top-left corner and it starts at 0,0 with the canvas size, so cel pixels are canvas pixels.
- **Keyframes.** A property with no keys has its base value (set with `element_add` or `element_set`). Once it has keys, its value at any frame comes from them: it holds the first key's value before it, the last key's after it, and in between travels along the **ease of the later key** (how the value moves *into* that key). The first key you give a property at a frame other than 0 also puts a key at frame 0 holding the value it had, so `element_set element=ball y=200 frame=12` alone makes the ball drop over frames 0-12.
- **Eases**: `linear`; `ease-in` (slow start, speeds up: falling, launching); `ease-out` (fast start, settles: landing, arriving); `ease-in-out` (slow at both ends: most natural moves); `step` (holds the old value, then jumps at the key: blinking, hold frames); `bezier` with `bezier=x1,y1,x2,y2`, the CSS `cubic-bezier` numbers, e.g. `0.34,1.56,0.64,1` overshoots and settles back.
- **Parenting.** `parent=<element>` makes an element move, turn and scale with another one, in that element's space. A bone's tip is at `x=length, y=0` in its own space, so a child at `x=<length>` sits on the tip. That's a cutout rig: bones for the skeleton, pictures or shapes parented to them, and keys on the bones' rotations. Chains can be as long as you like (up to 64), and loops are refused. 3D meshes can parent to other meshes.
- **3D layers** render each mesh with libs/mesh's preview renderer (lambert shading, outlines, 2x antialiasing) through a perspective camera. Without a camera, the layer frames every mesh from the front-right-top. Z is up; `x`, `y`, `z`, `rot_x`, `rot_y`, `rotation` (about z), `scale`, `scale_x/y/z`, `opacity` and `color` are all keyframeable, and so are the camera's position, `target_x/y/z` and `fov`.
- **Every edit is undoable** with `undo`, one step per command. Opening, looking (`info`, `key_list`, `values`, `preview`) and exporting don't change the animation and stay out of the history.
- **Documents live as long as the process.** For the CLI that's one command, so building an animation step by step happens in a batch script, over MCP, or by saving it with `animate.save` (an `.anim.json` file) and reopening it later with `animate.open`. Cel drawings are stored inside the file as PNG; images and mesh files are referred to by path.

## Task 1: a bouncing ball, as a GIF and a sprite sheet

A ball drops onto the floor with gravity (ease-in), squashes when it lands, and rises back (ease-out), in one second at 24 fps, so it loops.

**Batch.** `-> a` stores the new animation's id and `$a` uses it.

```text
# bounce.txt: a ball that drops, squashes and bounces back up, as a looping GIF
animate.new width=320 height=240 fps=24 frames=24 background=#f4f1ea -> a
animate.element_add doc=$a kind=rect width=320 height=40 x=160 y=220 color=#8a6f4d name=floor
animate.element_add doc=$a kind=ellipse width=50 height=50 x=160 y=60 anchor_y=1 color=#e4572e name=ball
animate.element_set doc=$a element=ball y=200 frame=12 ease=ease-in
animate.element_set doc=$a element=ball y=60 frame=23 ease=ease-out
animate.key_set doc=$a element=ball property=scale_y frame=11 value=1
animate.key_set doc=$a element=ball property=scale_y frame=12 value=0.7
animate.key_set doc=$a element=ball property=scale_y frame=14 value=1
animate.key_set doc=$a element=ball property=scale_x frame=11 value=1
animate.key_set doc=$a element=ball property=scale_x frame=12 value=1.3
animate.key_set doc=$a element=ball property=scale_x frame=14 value=1
animate.values doc=$a element=ball frame=6
animate.save doc=$a path=bounce.anim.json
animate.export_gif doc=$a output=bounce.gif
animate.export_sheet doc=$a output=bounce-sheet.png scale=0.5 step=2
animate.render doc=$a frame=12 output=bounce-12.png
```

`anchor_y=1` puts the ball's anchor at its bottom, so `y=200` means "resting on the floor" (the floor's top edge is at 200) and the squash keys flatten it onto the floor instead of around its middle.

```sh
$ wetwaresuite batch bounce.txt
created anim1 (320x240 24fps 24 frames (1s), 1 layers, 0 elements)
...
keyed at frame 12: e2 ellipse "ball" x=160 anchor_y=1 width=50 height=50 color=#e4572eff keys: y@0,12
...
e2 scale_y = 1 at frame 11 (linear); scale_y now has 2 keys (its first key, so frame 0 got a key with the value it had)
...
frame 6 (0.25s)
e2 (ball): x=160 y=104.15 rotation=0 scale=1 scale_x=1 scale_y=1 opacity=1 anchor_x=0.5 anchor_y=1 width=50 height=50 stroke_width=0 color=#e4572eff stroke=#1f2a37ff canvas=(160, 104.15)
...
saved bounce.anim.json
wrote bounce.gif (24 frames, 320x240, 26 KB)
packed 12 frames into bounce-sheet.png (480x480, 3 columns) + bounce-sheet.json
rendered frame 12 to bounce-12.png (320x240)
```

`animate.values` is how you check motion without looking: halfway through the fall (frame 6) the ball is at y=104, well short of the linear halfway point (130), because ease-in starts slowly.

**CLI.** An animation only lives inside one process, so from the shell you pipe lines into `wetwaresuite batch`. Re-exporting the saved file as a video and as a full-size sheet for a game:

```sh
$ printf 'animate.open path=bounce.anim.json -> a\nanimate.export_video doc=$a output=bounce.mp4\nanimate.export_sheet doc=$a output=sprites/ball.png name=bounce columns=6\n' | wetwaresuite batch
opened anim1 (320x240 24fps 24 frames (1s), 1 layers, 2 elements, bounce.anim.json)
...
encoded bounce.mp4 (24 frames at 24fps)
...
packed 24 frames into sprites/ball.png (1920x960, 6 columns) + sprites/ball.json
```

The atlas is the TexturePacker "hash" JSON that Phaser, PixiJS and most engine importers read (and `forge.sprite_split` can cut back into frames):

```json
{
  "frames": {
    "bounce_0000": {
      "frame": {"x": 0, "y": 0, "w": 320, "h": 240},
      "rotated": false, "trimmed": false,
      "spriteSourceSize": {"x": 0, "y": 0, "w": 320, "h": 240},
      "sourceSize": {"w": 320, "h": 240},
      "duration": 42
    },
    ...
  },
  "animations": {"bounce": ["bounce_0000", "bounce_0001", ...]},
  "meta": {"app": "wetwaresuite animate", "image": "ball.png", "format": "RGBA8888",
           "size": {"w": 1920, "h": 960}, "scale": "1", "frameRate": 24.0,
           "frameTags": [{"name": "bounce", "from": 0, "to": 23, "direction": "forward"}]}
}
```

`duration` is each frame's time in milliseconds; `frameTags` is the Aseprite-style tag some importers want.

**Claude.** *"Make a one-second looping animation of a red ball bouncing on a brown floor, with squash and stretch when it lands, and give me a GIF and a sprite sheet."* Claude calls `animate_new`, `animate_element_add` twice, `animate_element_set` with `frame` and `ease` for the fall and the rise, `animate_key_set` for the squash, then `animate_preview` with `frames: [0, 6, 12, 18]` to see the motion in one image before calling `animate_export_gif` and `animate_export_sheet`.

## Task 2: a logo sting with an overshoot and a fading caption

A picture slides in from the left with a springy overshoot, spins once, and a caption fades up and warms to the logo's colour. The logo is made with `paint` in the same script to keep the example self-contained; any PNG/JPEG works.

**Batch.**

```text
# logo.txt: a logo sting: a picture slides in with an overshoot, spins, and a caption fades up
paint.new width=200 height=200 background=transparent -> art
paint.shape doc=$art kind=ellipse x=10 y=10 width=180 height=180 color=#ffb703 filled=true
paint.shape doc=$art kind=rect x=70 y=70 width=60 height=60 color=#fb8500 filled=true
photo.save doc=$art path=logo.png
animate.new width=640 height=360 fps=30 frames=60 background=#023047 -> a
animate.element_add doc=$a kind=image source=logo.png width=160 x=-120 y=170 name=logo
animate.element_set doc=$a element=logo x=320 frame=20 bezier=0.34,1.56,0.64,1
animate.element_set doc=$a element=logo rotation=360 frame=40 ease=ease-in-out
animate.key_set doc=$a element=logo property=rotation frame=20 value=0
animate.element_add doc=$a kind=text text="ACME TOOLS" font_size=36 bold=true x=320 y=300 color=white opacity=0 name=caption
animate.key_set doc=$a element=caption property=opacity frame=30 value=0
animate.key_set doc=$a element=caption property=opacity frame=45 value=1 ease=ease-out
animate.key_set doc=$a element=caption property=color frame=59 value=#ffb703
animate.values doc=$a element=logo frame=14
animate.preview doc=$a frames=0,10,20,30,45,59
animate.export_video doc=$a output=sting.mp4 quality=best
animate.render doc=$a frame=50 output=sting-50.png
animate.save doc=$a path=sting.anim.json
```

```sh
$ wetwaresuite batch logo.txt
...
keyed at frame 20: e1 image "logo" source=logo.png (200x200) y=170 width=160 color=#ffffffff keys: x@0,20
keyed at frame 40: e1 image "logo" source=logo.png (200x200) y=170 width=160 color=#ffffffff keys: rotation@0,40 x@0,20
e1 rotation = 0 at frame 20 (linear); rotation now has 3 keys
...
frame 14 (0.467s)
e1 (logo): x=353.341 y=170 rotation=0 ... canvas=(353.341, 170)
frames 0,10,20,30,45,59 of anim1
encoded sting.mp4 (60 frames at 30fps)
rendered frame 50 to sting-50.png (640x360)
saved sting.anim.json
```

Things to notice:

- `width=160` alone sizes the 200x200 picture to 160x160 (the height follows the aspect ratio).
- At frame 14 the logo is at x=353, past its target of 320: that's the bezier overshoot, and it settles back by frame 20.
- The spin is keyed 0 at frame 20 and 360 at frame 40, so it starts only after the slide. (The `rotation=360 frame=40` call made a frame-0 key of 0 automatically; the extra key at 20 holds it still until then.)
- Colours interpolate channel by channel, alpha included, so `color=#ffffff00` to `#ffffffff` fades too.

**CLI.** Render one more frame of a saved sting, or change its length: `printf 'animate.open path=sting.anim.json -> a\nanimate.render doc=$a frame=30 output=check.png\n' | wetwaresuite batch`.

**Claude.** *"Animate logo.png sliding in from the left with a bit of overshoot, then one spin, and fade in 'ACME TOOLS' underneath. 2 seconds at 30 fps, mp4."* Claude adds the image and text with `animate_element_add`, keys them with `animate_element_set` (`frame`, `bezier`) and `animate_key_set`, checks the overshoot with `animate_values`, looks with `animate_preview` and exports with `animate_export_video`.

## Task 3: a cutout rig: an arm that waves

Bones are invisible joints. Shapes or pictures parented to them follow along, so you animate a few rotations instead of moving every piece.

**Batch.**

```text
# arm.txt: a cutout arm: upper arm and forearm bones, shapes parented to them, a wave
animate.new width=400 height=300 fps=24 frames=48 -> a
animate.element_add doc=$a kind=rect width=60 height=110 x=140 y=170 color=#3d5a80 name=body
animate.element_add doc=$a kind=bone x=165 y=130 length=80 rotation=-30 name=upper
animate.element_add doc=$a kind=bone parent=upper x=80 length=70 rotation=-40 name=lower
animate.element_add doc=$a kind=rect parent=upper width=90 height=24 anchor_x=0.06 x=0 y=0 color=#ee6c4d name=upper_skin
animate.element_add doc=$a kind=rect parent=lower width=80 height=20 anchor_x=0.06 x=0 y=0 color=#f4a261 name=lower_skin
animate.element_add doc=$a kind=ellipse parent=lower width=30 height=30 x=75 y=0 color=#f4a261 name=hand
animate.element_set doc=$a element=body order=front
animate.element_set doc=$a element=lower rotation=-100 frame=12 ease=ease-in-out
animate.element_set doc=$a element=lower rotation=-40 frame=24 ease=ease-in-out
animate.element_set doc=$a element=lower rotation=-100 frame=36 ease=ease-in-out
animate.element_set doc=$a element=lower rotation=-40 frame=47 ease=ease-in-out
animate.element_set doc=$a element=upper rotation=-60 frame=24 ease=ease-in-out
animate.element_set doc=$a element=upper rotation=-30 frame=47 ease=ease-in-out
animate.values doc=$a element=hand frame=12
animate.render doc=$a frame=12 bones=true output=arm-rig.png
animate.export_video doc=$a output=wave.mp4
animate.save doc=$a path=arm.anim.json
```

How the rig fits together:

- `upper` is the shoulder joint at 165,130 on the canvas. `lower` is its child at `x=80`, which is `upper`'s length, so the elbow sits on the shoulder bone's tip and turns with it.
- The skin rectangles are children of the bones at `x=0 y=0`, with `anchor_x=0.06` so the pivot sits just inside the end of each limb (like a pin through a paper puppet). The hand is a child of `lower` near its tip.
- Only the bones are keyed. Rotating `upper` swings the whole arm; rotating `lower` bends the elbow.
- `order=front` restacks the body above the arm pieces so the shoulder joint is hidden.

```sh
$ wetwaresuite batch arm.txt
...
keyed at frame 12: e3 bone "lower" parent=e2 x=80 y=0 length=70 color=#e0457bff keys: rotation@0,12
...
frame 12 (0.5s)
e6 (hand): x=75 y=0 rotation=0 ... canvas=(160.132, 30.413)
rendered frame 12 to arm-rig.png (400x300)
encoded wave.mp4 (48 frames at 24fps)
saved arm.anim.json
```

`animate.values` gives the hand's position on the canvas after the whole chain (`canvas=`), and for bones also the `tip=` point. `bones=true` (on `animate.render` and `animate.preview`) draws each bone as a diamond from its joint to its tip so you can see the rig.

To use pictures instead of shapes, add `kind=image source=forearm.png parent=lower` elements and set their anchors to where the joint is on the picture, e.g. `anchor_x=0.1 anchor_y=0.5`.

**CLI.** Check the rig of the saved file at the extreme pose: `printf 'animate.open path=arm.anim.json -> a\nanimate.preview doc=$a frame=24 bones=true\n' | wetwaresuite batch` (the preview is attached as a PNG; over the CLI you get its size; use `animate.render ... output=x.png bones=true` to get a file).

**Claude.** *"Build a simple cutout arm with an upper arm and forearm on bones and make it wave twice in two seconds. Show me the rig."* Claude adds two `bone` elements (the second with `parent`), parents shapes to them, keys the bones' `rotation`, and calls `animate_preview` with `bones: true` and `frames: [0, 12, 24, 36]`.

## Task 4: hand-drawn frames on a cel, with onion skin

A cel element is for traditional frame-by-frame animation. You draw on any frame; that drawing shows until the next drawing (so drawing on frames 0, 2, 4 is "animating on twos"). Onion skin shows the drawings before (red) and after (green) as faint ghosts while you work.

**Batch.**

```text
# flip.txt: a hand-drawn flipbook on a cel: three drawings on twos, onion skin, sprite sheet for a game
animate.new width=128 height=128 fps=12 frames=6 background=transparent -> a
animate.element_add doc=$a kind=cel name=ink -> cel
animate.draw doc=$a element=ink frame=0 mode=fill points=44,100,84,100,84,60,44,60 color=#2a9d8f
animate.draw doc=$a element=ink frame=0 points=44,60,64,30,84,60 size=5 color=#264653
animate.draw doc=$a element=ink frame=2 from_previous=true mode=erase points=50,62,78,62 size=10
animate.draw doc=$a element=ink frame=2 points=44,60,64,20,84,60 size=5 color=#264653
animate.draw doc=$a element=ink frame=4 mode=fill points=40,104,88,104,88,64,40,64 color=#2a9d8f
animate.draw doc=$a element=ink frame=4 points=40,64,64,40,88,64 size=5 color=#264653
animate.preview doc=$a frame=2 onion=1
animate.render doc=$a frame=2 onion=1 output=onion.png
animate.export_sheet doc=$a output=hut.png columns=6
animate.export_png doc=$a dir=frames
animate.save doc=$a path=hut.anim.json
```

```sh
$ wetwaresuite batch flip.txt
created anim1 (128x128 12fps 6 frames (0.5s), 1 layers, 0 elements)
added cel e1 on L1
filled on e1 at frame 0 (new drawing); drawings at 0
drew on e1 at frame 0; drawings at 0
erased on e1 at frame 2 (new drawing); drawings at 0,2
drew on e1 at frame 2; drawings at 0,2
filled on e1 at frame 4 (new drawing); drawings at 0,2,4
drew on e1 at frame 4; drawings at 0,2,4
frame 2 of anim1
rendered frame 2 to onion.png (128x128)
packed 6 frames into hut.png (768x128, 6 columns) + hut.json
wrote 6 frames to frames (frame_0000.png ...)
saved hut.anim.json
```

- `mode=stroke` (the default) paints a brush stroke through the points (`size`, `hardness`, `opacity`, `pressure=` one value per point to taper it, `smooth=` to steady shaky input); `mode=fill` fills the closed shape; `mode=erase` rubs out.
- Drawing on a frame with no drawing starts a blank one; `from_previous=true` starts from a copy of the drawing showing (for small changes: here frame 2 keeps the walls and redraws the roof).
- `animate.cel` manages drawings: `action=blank` (an empty drawing, so nothing shows from there), `action=delete` (the previous drawing holds again), `action=copy from=0` (reuse a drawing, for cycles), `action=import path=drawing.png` (one image) or `path=folder/` (every image in name order, `hold=2` for twos).
- A cel is also an ordinary element: move, scale, fade or tint (`color`) it with keys, or parent it to a bone.

**CLI.** Bring in frames drawn elsewhere and make a GIF: `printf 'animate.new width=256 height=256 fps=12 frames=24 -> a\nanimate.element_add doc=$a kind=cel -> c\nanimate.cel doc=$a element=$c action=import frame=0 path=sketches hold=2\nanimate.export_gif doc=$a output=sketches.gif\n' | wetwaresuite batch`.

**Claude.** *"Draw a little hut on a 128 px transparent canvas that stretches up and settles over three drawings on twos, show me frame 2 with onion skin, then export a sprite strip."* Claude calls `animate_element_add` with `kind: cel`, `animate_draw` per stroke and fill, `animate_preview` with `onion: 1` to compare drawings, then `animate_export_sheet` with `columns: 6`.

## Task 5: a 3D turntable with a title

A torus spins on a stand while the camera pushes in, and a 2D title layer fades in on top.

**Batch.**

```text
# turntable.txt: a 3D turntable: a torus spinning on a box, camera pushing in, title on top
animate.new width=480 height=270 fps=24 frames=48 layer=3d background=#1d2433 -> a
animate.element_add doc=$a kind=mesh source=box size=3,3,0.4 z=-0.2 color=#5c677d name=stand
animate.element_add doc=$a kind=mesh source=torus size=1,0.35 z=0.9 rot_x=90 color=#f2a541 name=ring
animate.element_add doc=$a kind=camera x=0 y=-8 z=3 target_z=0.6 fov=35 name=cam
animate.element_set doc=$a element=ring rotation=360 frame=47
animate.element_set doc=$a element=cam y=-6 z=2.2 frame=47 ease=ease-in-out
animate.layer_add doc=$a kind=2d name=titles
animate.element_add doc=$a kind=text layer=titles text="SPIN" font_size=40 bold=true x=240 y=36 color=white opacity=0
animate.element_set doc=$a element=e4 opacity=1 frame=12
animate.preview doc=$a frames=0,16,32,47
animate.export_video doc=$a output=turntable.webm quality=draft
animate.export_gif doc=$a output=turntable.gif scale=0.5
animate.render doc=$a frame=20 output=turntable-20.png
animate.save doc=$a path=turntable.anim.json
```

```sh
$ wetwaresuite batch turntable.txt
created anim1 (480x270 24fps 48 frames (2s), 1 layers, 0 elements)
added mesh e1 on L1
added mesh e2 on L1
added camera e3 on L1
keyed at frame 47: e2 mesh "ring" source=torus size=1,0.35 x=0 y=0 z=0.9 rot_x=90 color=#f2a541ff keys: rotation@0,47
keyed at frame 47: e3 camera "cam" x=0 target_z=0.6 fov=35 keys: y@0,47 z@0,47
added 2d layer L2 "titles"
added text e4 on L2
keyed at frame 12: e4 text text="SPIN" x=240 y=36 font_size=40 color=#ffffffff keys: opacity@0,12
frames 0,16,32,47 of anim1
encoded turntable.webm (48 frames at 24fps)
wrote turntable.gif (48 frames, 240x135, 196 KB)
...
```

- Primitive sizes: box `x,y,z` (one number = a cube), sphere `radius`, cylinder `radius,height`, cone `bottom_radius,top_radius,height`, torus `major,minor` radius, plane `width,depth`. Defaults are about one unit across.
- Rotations apply x, then y, then z (`rotation` is about z, the up axis), so `rot_x=90` stands the ring up and keying `rotation` spins it like a turntable.
- The camera looks from its position towards `target_x/y/z`; both, and `fov`, can be keyed (push-ins, orbits, zooms). Without a camera the layer frames all its meshes automatically.
- Use your own models with `source=part.stl` (or OBJ, PLY, OFF, glTF/GLB, 3MF), for example a part exported from `cad`.
- A 3D layer is just a layer: 2D layers above or below it composite as usual, so titles, logos and backgrounds mix with 3D.

**CLI.** Animate an existing model in a few lines: `printf 'animate.new layer=3d -> a\nanimate.element_add doc=$a kind=mesh source=bracket.stl -> m\nanimate.element_set doc=$a element=$m rotation=360 frame=47\nanimate.export_gif doc=$a output=bracket.gif scale=0.5\n' | wetwaresuite batch` (no camera, so the view frames the model).

**Claude.** *"Make a 2-second turntable of bracket.stl on a dark background with the camera slowly pushing in, as a webm."* Claude creates a `3d` animation, adds the mesh and a camera with `animate_element_add`, keys `rotation` and the camera's position with `animate_element_set`, checks with `animate_preview` (`every: 12`) and calls `animate_export_video`.

## Checking and fixing an animation

```text
# fix.txt
animate.open path=bounce.anim.json -> a
animate.info doc=$a
animate.key_list doc=$a element=ball
animate.element_set doc=$a element=ball y=100
animate.key_delete doc=$a element=ball property=scale_x frame=12
undo
animate.element_set doc=$a element=ball color=#3a86ff
animate.layer_add doc=$a name=shadow
animate.element_add doc=$a kind=ellipse layer=shadow width=60 height=10 x=160 y=200 color=#00000040
animate.layer_set doc=$a layer=shadow index=0
animate.settings doc=$a frames=48
```

```sh
$ wetwaresuite batch --keep-going fix.txt
opened anim1 (320x240 24fps 24 frames (1s), 1 layers, 2 elements, bounce.anim.json)
anim1: 320x240 24fps, frames 0..23 (1s), background #f4f1eaff
L1 "Layer 1" (2d)
  e2 ellipse "ball" x=160 anchor_y=1 width=50 height=50 color=#e4572eff keys: scale_x@0,11,12,14 scale_y@0,11,12,14 y@0,12,23
  e1 rect "floor" x=160 y=220 width=320 height=40 color=#8a6f4dff
(layers and elements are listed top first)
e2 (ball) scale_x: 0=1 11=1 12=1.3 14=1
e2 (ball) scale_y: 0=1 11=1 12=0.7 14=1
e2 (ball) y: 0=60 12=200 ease-in 23=60 ease-out
line 4: y is keyframed (3 keys), so a base value wouldn't show; pass frame=N to set a key there, or animate.key_delete element=e2 property=y all=true first
removed 1 key from e2
undone
...
added 2d layer L2 "shadow"
added ellipse e3 on L2
layer L2 "shadow", opacity 1, normal
animation is now 320x240 24fps 48 frames (2s), 2 layers, 3 elements, bounce.anim.json
```

- `animate.info` lists every layer and element (top first) with the properties that differ from defaults and which properties are keyed at which frames.
- `animate.key_list` shows each key's value and ease; `animate.values` shows every property at one frame after easing, plus where the element lands on the canvas.
- Setting a keyed property without `frame` is refused (the base value would never show); key it, or delete its keys first. `key_delete` with `all=true` and no `property` clears every key of an element.
- `animate.layer_set index=0` moves a layer to the bottom (`-1` is the top); `animate.element_set order=front|back|forward|backward` restacks an element within its layer, and `layer=` moves it to another layer of the same kind.
- `animate.settings frames=48` makes the animation longer; keys keep their frame numbers, so the new frames hold the last values.

## Looking over MCP

Every command is an MCP tool (`animate.key_set` is `animate_key_set`). The ones Claude uses to see:

- `animate_preview`: one frame as an image, or `frames: [0, 6, 12, 18]` / `every: 6` for a contact strip with frame numbers, which shows a whole motion in one picture. `onion` and `bones` work here too.
- `preview: true` on any edit (`element_add`, `element_set`, `key_set`, `draw`, `cel`, `layer_set`, `settings`) attaches the frame that edit is about (its `frame`, else frame 0).
- `animate_values` for exact numbers, `animate_info` and `animate_key_list` for structure.
- The open animation is also an MCP resource (`wetwaresuite://doc/anim1`) whose preview is frame 0.

A typical exchange: *"Make the ball in anim1 hang in the air for a moment at the top."* Claude reads `animate_key_list`, adds `animate_key_set` keys on `y` at frames 20 and 23 with the same value (a hold) and `ease: "ease-out"` on the first, then checks `animate_preview` with `frames: [18, 20, 22, 23]`.

## Exports

| command | writes | notes |
| --- | --- | --- |
| `animate.render` | one frame as PNG/JPG/BMP/TGA | `scale`, `onion`, `bones` |
| `animate.export_png` | `dir/frame_0000.png`, ... | transparency kept; `prefix` |
| `animate.export_sheet` | sheet PNG + JSON atlas | grid of `columns` (default near-square), `padding`, `name`; at most 16384 px a side |
| `animate.export_gif` | animated GIF | built-in encoder, per-frame 256-colour palettes, `loop` (0 = forever), `dither`, transparency (pixels under half alpha) |
| `animate.export_video` | .mp4 / .mkv (H.264), .webm (VP9), .mov (ProRes) | needs ffmpeg; `quality=draft|good|best`; webm and mov keep a transparent background, mp4/mkv put it on white; odd sizes are padded to even |

All exports take `start`, `end` (inclusive), `scale` and, except video, `step` (every Nth frame: the sheet's durations and the GIF's delays stretch to match, so the speed stays right). Exports replace existing files unless `overwrite=false`.

## Command reference

### Documents and layers

| command | what it does | key params |
| --- | --- | --- |
| `animate.new` | new animation (with layer L1) | `width` 640, `height` 360, `fps` 24, `frames` 48, `background` white, `layer` 2d/3d/none |
| `animate.open` / `animate.save` | .anim.json files | `path` |
| `animate.close` | close (undoable) | `doc` |
| `animate.info` | describe everything | `doc` |
| `animate.settings` | canvas, fps, length, background | `width`, `height`, `fps`, `frames`, `background` |
| `animate.layer_add` | new layer on top | `kind` 2d/3d, `name`, `opacity`, `blend` |
| `animate.layer_set` | change a layer | `layer`, `name`, `visible`, `opacity`, `blend`, `index` |
| `animate.layer_remove` | delete a layer and its elements | `layer` |

### Elements, keys and cels

| command | what it does | key params |
| --- | --- | --- |
| `animate.element_add` | add an element (returns e<n>) | `kind`, `layer` (default: top layer of the right kind, made if missing), any property |
| `animate.element_set` | change properties; with `frame`, key them | `element`, `frame`, `ease`, `bezier`, `order`, `layer`, any property |
| `animate.element_remove` | delete (children are detached) | `element` |
| `animate.key_set` | one key | `element`, `property`, `frame`, `value` (number or colour), `ease`, `bezier` |
| `animate.key_delete` | remove keys | `element`, `property`, `frame` or `all=true` |
| `animate.key_list` | list keys | `element`, `property` (both optional) |
| `animate.values` | evaluate at a frame | `element` (optional), `frame` |
| `animate.draw` | draw on a cel | `element`, `frame`, `points`, `mode` stroke/fill/erase, `color`, `size`, `hardness`, `opacity`, `pressure`, `smooth`, `from_previous` |
| `animate.cel` | blank / delete / copy / import drawings | `element`, `action`, `frame`, `from`, `path`, `hold` |
| `animate.preview` | look (attached PNG) | `frame`, `frames`, `every`, `onion`, `bones`, `size` |

### Element properties

All number and colour properties can be keyframed.

| property | kinds | meaning (default) |
| --- | --- | --- |
| `x`, `y` | all | 2D: anchor position in px (0); 3D: world position (0) |
| `z` | mesh, camera | world height (0; camera 4) |
| `rotation` | 2D, mesh | degrees, clockwise in 2D, about z in 3D (0) |
| `rot_x`, `rot_y` | mesh | degrees about x, then y (0) |
| `scale`, `scale_x`, `scale_y` | 2D, mesh | multipliers (1); negative mirrors |
| `scale_z` | mesh | (1) |
| `opacity` | all but bone, camera | 0..1 (1) |
| `anchor_x`, `anchor_y` | rect, ellipse, text, image, cel | pivot as a fraction of the box (0.5; cel 0) |
| `width`, `height` | rect, ellipse, image, cel | box size in px (100; image natural; cel canvas) |
| `stroke_width` | rect, ellipse, polygon, line | outline / line width (0; line 4) |
| `font_size` | text | px (32) |
| `length` | bone | px (100) |
| `target_x/y/z`, `fov` | camera | look-at point (0,0,0), field of view in degrees (40) |
| `color` | all but camera | fill / text / line / mesh colour, tint for image and cel (white = unchanged) |
| `stroke` | rect, ellipse, polygon | outline colour |

Other element params (not keyframeable): `name`, `parent`, `visible`, `points`, `text`, `font` (sans/serif/mono), `bold`, `italic`, `source`, `size`. Colours are names (`red`, `transparent`), `#rgb`, `#rrggbb`, `#rrggbbaa` or `rgb(r,g,b)`.

## Limitations

- Shapes are filled polygons and outlines are brush strokes, so very thick outlines have soft round joins; there are no gradients, rounded corners or masks yet.
- Text uses the suite's built-in preview fonts (FreeSans, FreeSerif, FreeMono shapes on Helvetica/Times/Courier metrics), Latin-1 only, left-aligned. Big scale-ups of text are re-rendered at the larger size, but the glyphs come from a fixed-size bitmap, so very large text is slightly soft.
- Pictures and cels are sampled bilinearly (pictures shrunk more than 2x are pre-scaled); there is no motion blur.
- Parenting carries position, rotation and scale, not opacity or visibility. 2D and 3D elements can't parent to each other.
- 3D is a preview-quality renderer: one light setup, flat colours per mesh, outlines on, no textures, shadows or transparency inside a mesh. Meshes are drawn one at a time far to near (by their centres), so meshes that pass through each other can show the wrong one in front. The camera has no roll.
- Interpolation is per number: rotations go the long way if you key 350 then 10 (key 370 instead), and colours blend in RGB.
- The GIF encoder makes one palette per frame (no global palette or frame differencing), so files are bigger than a dedicated optimiser would make and gradients can band (try `dither=true`). Transparency is on/off at half alpha.
- Video export writes every frame as a temporary PNG first, and needs ffmpeg with libx264 (mp4/mkv), libvpx-vp9 (webm) or prores_ks (mov).
- Images and mesh files are saved by path, not embedded: move them along with the `.anim.json` (paths relative to the file's folder are resolved on open, absolute paths are kept as they are).
