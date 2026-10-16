# The wetwaresuite window

wetwaresuite has a desktop window for people who would rather click than type. It sits on top of the
same commands the command line, batch scripts and Claude use, so anything you do in the window can
also be done in a script, and every change you make can be undone.

The window works completely offline. It needs no internet connection, no account and no AI. It also
needs no graphics card: on a computer without one it draws in software.

![The image app with a picture open and the Adjust colours form on the right](images/gui-image-app.jpg)

## 1. Start it

If you built wetwaresuite from source (see [Getting started](getting-started.md)), the window program
is in your build folder:

```sh
./build/gui/wetwaresuite-gui                 # every tool
./build/gui/wetwaresuite-gui --app image     # just photo editing and painting
./build/gui/wetwaresuite-gui --app image holiday.jpg   # and open a picture straight away
```

On Windows the program is `build\gui\Release\wetwaresuite-gui.exe`. You can also drag a file onto
the program's icon, or onto the open window, to open it.

`--app` picks which tools the window shows. `wetwaresuite-gui --list-apps` lists them:

| app | what it is for |
|-----|----------------|
| `all` | every tool (the default) |
| `image` | photo editing and painting |
| `vector` | vector drawing and changing file types |
| `3d` | 3D parts, models, renders and 3D printing |
| `sound` | sound editing and music |
| `office` | documents, page layouts and notes |

Or name the tools yourself: `wetwaresuite-gui --tools photo,convert`.

## 2. The home screen

When the window opens you see the home screen:

- **Open a file** shows your computer's normal "open" window. Pick a picture (or any file the app
  understands) and it opens.
- **Start something new** lists the things you can create, such as a blank canvas to paint on.
- **Find an action** jumps to the search box (see below).
- **Recent files** lists what you opened before. Click one to open it again.
- One **card per tool** says in a sentence what it does, with buttons for its most common actions.

You can always get back here with the **Home** button at the top left.

## 3. Find any action by typing

Click the search box at the top, or press **Ctrl+K** (Cmd+K on a Mac), and type what you want to do
in your own words: `resize`, `brightness`, `crop`, `blur`, `layer`. Matching actions appear as you
type. You don't have to spell them exactly: `brsh` finds "Brush stroke". Use the arrow keys and
**Enter**, or click one.

## 4. Fill in the form and press Run

Picking an action, from search, the list on the left or a card, shows its form on the right. Every
box is already filled with a sensible value, so often you only press **Run** (or **Ctrl+Enter**).

- **Numbers with a known range** (like opacity 0 to 100) are sliders. Drag them, or Ctrl+click one
  to type a value.
- **Optional numbers** start ticked as **Automatic**: the tool chooses. Untick it to choose yourself.
- **Yes/no** settings are tick boxes, and **choices** are drop-down lists.
- **Colours** have a colour swatch: click it to pick a colour. You can also type `#ff0000` or a name
  like `red`.
- **Files and folders** have a **Browse...** button that shows your computer's normal file window. You
  can also type or paste a path.
- **Document** picks which open file the action works on. It starts on the file you are looking at.

After you press Run, the result shows under the form: **Done** in green with what happened, or
**That didn't work** in red with the reason in plain words, for example *Please fill in "Height".*
Nothing changes when an action fails.

## 5. Simple and Pro

The left side lists actions for each tool. In **Simple** mode (the default) it shows only the most
common ones, about eight per tool, and every box in a form has a short explanation under it.

Press **Mode: Simple** at the top to switch to **Pro**: every action of every tool is listed, and
the explanations move into tooltips (hover over a box's name to read it).

## 6. Looking at your picture

When a file is open, the middle of the window shows it.

- **Scroll** the mouse wheel to zoom in and out around the mouse pointer.
- **Drag** with the right or middle mouse button (or hold the **Space** bar and drag) to move around.
- **Fit** shows the whole picture, **100%** shows it at its real size.
- The list of **Open files** on the left switches between files.

Files that aren't pictures (a 3D model, a song, a map) show their preview picture if the tool has one,
or a one-line summary.

## 7. Painting

In the image app (or any app with the paint tool), pictures get painting tools above the canvas:

1. Choose **Brush**.
2. Click the colour swatch next to it and pick a colour.
3. Pick a **Brush** style (pen, pencil, ink, marker, airbrush, soft, round), a **Size** and an
   **Opacity**.
4. Draw on the picture with the mouse. You see the line while you draw; when you let go it is painted
   for real.

With a **drawing tablet or pen screen**, pressing harder paints thicker or darker lines (depending on
the brush), and flipping the pen over to its eraser end rubs out. **Eraser** rubs out with the mouse.
**Fill** fills the area you click with the colour, like a paint bucket.

Each stroke is one step in the history, so Ctrl+Z takes back exactly one stroke.

To start a fresh canvas: **Start something new**, then **New canvas (Paint)**, set the width and
height, and press Run. To keep your work, search for **save**, pick **Save image**, press
**Browse...** to choose where, and Run. Saving as `.ora` keeps the layers.

## 8. Undo, redo and history

- **Undo** (Ctrl+Z) takes back the last change. **Redo** (Ctrl+Y or Ctrl+Shift+Z) puts it back.
- The **History** list on the right shows every change in order; the last one is marked `<- now`.

Opening, looking and saving are not changes, so they don't appear in the history.

## 9. Text size and colours

Press **Aa** at the top right:

- **Text size** makes everything bigger or smaller (the window already follows your screen's scale
  setting).
- **Dark** or **Light** colours. Both are high contrast.

## 10. Doing the same thing without the window

Every button runs a command, and the same command works from the command line. For example,
painting a stroke in the window does what this does:

```sh
wetwaresuite run paint.stroke doc=img1 points="[10,10, 50,40, 90,10]" color="#ff0000" size=12
```

The action's name is shown next to it in the search results (for example `photo.resize`), so
you can find it in `wetwaresuite help photo.resize` and use it in [batch scripts](headless-and-batch.md).

The window program can also serve its tools to Claude, just like `wetwaresuite mcp`:

```sh
wetwaresuite-gui mcp --app image
```

This is optional: nothing in the window needs it.

## 11. When something goes wrong

- **"Couldn't show a file window"**: on Linux the Browse button uses your desktop's file window
  (through the desktop portal or `zenity`). If neither is installed, type or paste the path into
  the box instead.
- **"Couldn't start the window"** or **"was built without a window"**: the computer has no screen
  (a server), or wetwaresuite was built without the window (`-DWETWARESUITE_GUI=OFF`, or SDL3 couldn't be
  downloaded when it was built). Everything still works from the command line.
- On Linux, building the window needs the X11 or Wayland development files (`libx11-dev` on
  Debian/Ubuntu). Without them it still builds, but can only draw offscreen.

## For builders

- `cmake -S . -B build` builds the window by default. `-DWETWARESUITE_GUI=OFF` leaves it out.
- SDL3 (zlib license) is used from your system if CMake finds it, else the pinned SDL3 3.2.24 release
  is downloaded once while configuring. With no network, set
  `-DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL3-3.2.24` to an unpacked copy, or the window is
  skipped and a stub is built in its place. Nothing is downloaded when the program runs.
- Dear ImGui (MIT) is vendored in `third_party/imgui`.
- `wetwaresuite-gui --screenshot shot.bmp` draws a few frames, saves them and quits. With
  `SDL_VIDEO_DRIVER=offscreen` it works on a machine with no screen, which is how the picture above
  was made.
- Apps link `wetwaresuite_gui` and call `wetwaresuite::gui::run(session, {name, tools, accent})`.
