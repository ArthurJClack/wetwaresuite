# The apps: install only what you need

wetwaresuite comes as a set of small apps. Each app is one program that does one kind of job, and you only install the ones you want. They all work without the internet and without an AI connected.

If you want everything in one program (handy for scripts, servers and Claude), there is also `wetwaresuite`, the all-in-one binary. It has every command of every app.

## Which app does what

| app | program | what it's for | tools inside | size |
| --- | --- | --- | --- | --- |
| image | `wetwaresuite-image` | edit photos, paint pictures, make textures | photo, paint | ~2 MB |
| draw | `wetwaresuite-draw` | vector art and logos, fonts, whiteboards and diagrams | vector, type, board | ~5 MB |
| model | `wetwaresuite-model` | 3D modeling and sculpting, rendered pictures, animation | sculpt, render, animate | ~4 MB |
| game | `wetwaresuite-game` | game assets (meshes, sprites, textures) and game levels | forge, levels | ~4 MB |
| cad | `wetwaresuite-cad` | precise parts, technical drawings and floor plans, 3D printing, car parts and mounts that fit a phone 3D scan | cad, blueprint, slicer, fit | ~5 MB |
| electronics | `wetwaresuite-electronics` | circuit diagrams, circuit boards, flashing microcontrollers | circuit, flash | ~4 MB |
| sound | `wetwaresuite-sound` | record and clean up audio, make music | audio, beats | ~4 MB |
| video | `wetwaresuite-video` | edit videos, screenshots, screen recording | cut, capture | ~3 MB |
| office | `wetwaresuite-office` | documents, spreadsheets, slides, PDFs, print layouts, notes | office, layout, notes | ~7 MB |
| maps | `wetwaresuite-maps` | offline maps, GPS tracks and map data | maps | ~4 MB |
| system | `wetwaresuite-system` | calculator, code editor, file manager, terminal, system monitor and clock | calc, code, explorer, terminal, monitor, clock | ~6 MB |
| files | `wetwaresuite-files` | open, convert, find and batch-process any file | toolkit, convert, library, automate | ~4 MB |
| share | `wetwaresuite-share` | share and live-sync folders with other devices | share | ~1 MB |

`files` is small and always comes along when you install any other app: it is the one that says what a file is, converts between formats and finds things in your folders.

Every tool has its own tutorial (for example [photo](photo.md) or [sculpt](sculpt.md)); the commands are the same whichever program they are in. `wetwaresuite-image photo.resize ...` and `wetwaresuite photo.resize ...` do exactly the same thing.

## Install only what you need

The `hub` program installs apps. It never downloads anything while installing: you point it at a folder or archive you already have, such as a release download, an [offline edition](offline-editions.md) on a USB stick, or your own build folder.

1. See what there is:

   ```sh
   hub list
   ```

   You get each app, its size, whether it's installed, and the tools inside it.

2. Install the apps you want, from the folder or archive you have:

   ```sh
   hub install image model --from ~/Downloads/wetwaresuite-release
   hub install cad --from ~/Downloads/wetwaresuite-release.zip
   ```

   Tool names work too: `hub install paint` installs the image app, `hub install slicer` installs cad. `hub install all` installs every app, and `hub install suite` installs the all-in-one `wetwaresuite`.

3. The apps go into your personal folder (nothing system-wide, no admin rights needed):

   | system | folder |
   | --- | --- |
   | Linux | `~/.local/share/wetwaresuite/bin` |
   | macOS | `~/Library/Application Support/wetwaresuite/bin` |
   | Windows | `%APPDATA%\wetwaresuite\bin` |

   Add that folder to your PATH to run the apps by name, or pass `--to <folder>` to put them somewhere else. Setting the `WETWARESUITE_HOME` environment variable moves the whole wetwaresuite folder.

4. To remove one: `hub uninstall model`.

You can also skip the hub completely: the apps are plain programs, so copying `wetwaresuite-image` anywhere and running it works.

## Using an app

Double-click an app (or start it with no arguments) and it opens its window; drag a file onto it to
open that file. The window is explained in [the window (GUI)](gui.md), and passing work between
app windows in [workflows](workflows.md#with-the-windows-gui).

Every app also has the same command line:

```sh
wetwaresuite-image list                     # every command in this app
wetwaresuite-image help photo.resize        # what a command takes
wetwaresuite-image photo.resize ...         # run one command (same as `run photo.resize ...`)
wetwaresuite-image batch script.wws         # run a script, one command per line
wetwaresuite-image mcp                      # let Claude drive it (optional)
wetwaresuite-image app.installed            # which other apps you have
```

If you ask an app for a command another app has, it tells you which app to use:

```
$ wetwaresuite-image sculpt.new
unknown command: sculpt.new (try `wetwaresuite-image list`; sculpt is in wetwaresuite-model)
```

Moving work between apps (a texture from image onto a model, a model into a game) is covered step by step in [workflows](workflows.md).

## Building only some apps from source

By default a build makes every app plus the all-in-one binary. To build just a few (faster, and only the code those apps need is compiled):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWETWARESUITE_APPS="image;model" -DWETWARESUITE_BUILD_SUITE=OFF
cmake --build build
```

The apps land together in `build/bin/`. Each app also has its own switch, for example `-DWETWARESUITE_APP_VIDEO=OFF`. Each app program links only its own tools, so `wetwaresuite-image` is about 2 MB while the all-in-one binary is over 20 MB.

## For Claude (optional)

Connect only the apps you use, or the all-in-one binary for everything:

```sh
claude mcp add wetwaresuite-image -- wetwaresuite-image mcp
claude mcp add wetwaresuite-model -- wetwaresuite-model mcp
```

`hub mcp-config` prints a ready-made config with one entry per installed app (or just `wetwaresuite` if the all-in-one binary is installed, since it covers everything). Each app tells Claude which other apps are installed and how to hand work to them, so Claude can, for example, paint a texture in image and then pick it up in model.
