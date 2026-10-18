# Levels: game levels and tilemaps

`levels` builds 2D game levels without a GUI. A level is a tile map: a grid of cells, each holding a tile cut from a tileset image, stacked in layers, plus object layers for things that aren't tiles (the player's spawn point, enemies, chests, doors, trigger zones). You can paint tiles one by one, fill, flood, draw lines, stamp blocks, auto-tile edges, scatter decoration at random, or generate whole caves, dungeons and overworlds from a seed. Levels go out as Tiled TMX/TMJ (which Godot, Unity, Phaser, Defold, LÖVE, MonoGame and most engines import), a plain JSON file for your own loader, LDtk-lite JSON, CSV per layer, or a flat PNG.

Each task below is shown three ways:

- **CLI**: `wetwaresuite <command> key=value` runs one command in a fresh process. A level only lives as long as the process, so one-shot commands are for opening and inspecting files; for edits, pipe a few lines into `wetwaresuite batch` from the shell.
- **Batch**: a script file run with `wetwaresuite batch file.txt`. `-> name` at the end of a line saves the result (the level id) and `$name` reuses it; `#` starts a comment; `undo` and `redo` work as lines (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools levels` serves just this tool; add `forge` when you also want sprite sheets packed.

Every example was run against the real binary, in an empty folder; outputs are copied from those runs (long ones shortened where marked with `...`).

## Contents

1. [The ideas you need](#1-the-ideas-you-need)
2. [Task: a cave level from nothing](#2-task-a-cave-level-from-nothing)
3. [Task: a dungeon with rooms, entities and collision for your own engine](#3-task-a-dungeon-with-rooms-entities-and-collision-for-your-own-engine)
4. [Task: an overworld from noise, on a tileset packed by forge](#4-task-an-overworld-from-noise-on-a-tileset-packed-by-forge)
5. [Task: hand edits on an existing Tiled map](#5-task-hand-edits-on-an-existing-tiled-map)
6. [Task: inspect and convert map files](#6-task-inspect-and-convert-map-files)
7. [Auto-tiling in detail](#7-auto-tiling-in-detail)
8. [Objects, properties and collision](#8-objects-properties-and-collision)
9. [File formats](#9-file-formats)
10. [Command reference](#10-command-reference)
11. [Limitations](#11-limitations)

## 1. The ideas you need

**Cells.** A map is `width` x `height` cells, each `tile_width` x `tile_height` pixels. Cells are addressed `x` (column) and `y` (row), counted from 0 at the top-left. Object positions are in pixels from the same corner, like Tiled; most object commands also take `cell=column,row`.

**Tilesets and tile ids.** `levels.tileset_add` slices an image into tiles numbered 0, 1, 2... left to right, then top to bottom, honouring `margin` (border around the sheet) and `spacing` (gap between tiles). Commands take `tile=<id within the tileset>` and `tileset=<name or index>` (default: the first tileset); `tile=-1` means empty. Internally, and in TMX/TMJ files, cells store Tiled's global ids (gids): 0 is empty and tileset T's tile n is `T.firstgid + n`. `levels.info` shows each tileset's range.

No art yet? `levels.tileset_placeholder` draws a tileset image for you (distinct colours, or a 16-tile auto-tile template), and a map with no tileset at all still works: tile n draws as a flat colour.

**Layers.** Layers draw bottom first. Tile layers hold one tile per cell; object layers hold objects. A tile layer can be marked as a *collision grid* (`collision=true`): its cells are 0/1, it renders as a red overlay in previews, and it is exported as an IntGrid in LDtk and as a `collision=true` layer property in Tiled. Commands that paint take `layer=<name or index>`; without it they use the top tile layer that isn't a collision grid.

**Objects.** An object has an id, a name, a type (Tiled's "class"), a position, a size, a shape (`rect`, `ellipse` or `point`) and custom properties (`hp=10; loot=gold`, types inferred: bool, int, float, colour, or string).

**Undo and previews.** Every edit is one undo step (`undo` in a batch script or the `undo` MCP tool); a command that fails changes nothing. Every edit takes `preview=true`, which attaches a PNG render of the map to the result, so Claude sees what it just did. `levels.preview` renders without changing anything, with options for a grid, objects and which layers to show.

**Randomness.** Scatter and the generators take `seed=`. The same seed gives the same map on every platform (the suite uses its own random number generator rather than the C++ library's, whose results differ between compilers).

## 2. Task: a cave level from nothing

The goal: a 48x32 cave for a platformer prototype, with rock edges that look right, a few props on the floor, a spawn point, collision rectangles for the physics engine, a Tiled file and a picture.

### Batch script

```text
# cave.txt: a cave level from nothing, with placeholder art
levels.new width=48 height=32 tile_width=16 layer=walls background=#1d1b26 -> m
levels.tileset_placeholder doc=$m path=art/rock.png style=autotile colors=#7a6a58 name=rock
levels.tileset_placeholder doc=$m path=art/props.png tiles=8 columns=4 name=props
levels.gen_caves doc=$m layer=walls wall=15 tileset=rock floor=-1 fill=0.46 steps=5 seed=11
levels.autotile doc=$m layer=walls base=0 tileset=rock
levels.layer_add doc=$m name=props
levels.scatter doc=$m layer=props tiles=2,3,4 weights=3,1,1 tileset=props density=0.06 avoid_layer=walls seed=4
levels.object_add doc=$m layer=entities type=spawn name=start shape=point cell=10,16
levels.find doc=$m layer=walls tile=-1 limit=3
levels.collision doc=$m from=walls
levels.info doc=$m
levels.save doc=$m path=out/cave.tmx
levels.save doc=$m path=out/cave.png
```

```sh
$ wetwaresuite batch cave.txt
created lvl1: 48x32 cells of 16x16 px, 1 tile layer, 0 object layers, 0 objects, 0 tilesets
...
drew 16 autotile tiles to art/rock.png and added tileset rock (gids 1..16)
...
drew 8 colors tiles to art/props.png and added tileset props (gids 17..24)
...
generated caves on walls: 790 floor cells (51%)
...
auto-tiled 214 cells on walls with tiles rock:0..rock:15
...
scattered 54 tiles on props
...
added object #1 start [spawn] at 168,264 point on entities
...
790 cells of empty
  walls: 790
positions: 5,2 6,2 7,2 ...
...
built 73 collision rectangles covering 746 solid cells on collision
...
48x32 cells of 16x16 px, 2 tile layers, 2 object layers, 74 objects, 2 tilesets
tilesets:
  0: rock tiles 0..15 (gids 1..16), 4 columns of 16x16, /home/me/game/art/rock.png
  1: props tiles 0..7 (gids 17..24), 4 columns of 16x16, /home/me/game/art/props.png
layers (bottom first):
  0: walls (tiles, 746 cells filled)
  1: props (tiles, 54 cells filled)
  2: entities (objects, 1 object)
  3: collision (objects, 73 objects)
...
saved out/cave.tmx (tmx)
...
saved out/cave.png (png)
```

What each step does:

1. **`levels.new`** makes the map with its first tile layer called `walls` and a dark background colour (used by renders; Tiled shows it too).
2. **`levels.tileset_placeholder style=autotile`** draws the 16 tiles of the 4-bit auto-tile rule into `art/rock.png` (tile n has a dark rim on each side that has no rock neighbour) and adds it as tileset `rock`. The second one draws 8 plain colours for props. Replace the PNGs with real art later: the map refers to the files, so new pixels show up without touching the level.
3. **`levels.gen_caves`** fills `walls` with tile 15 (rock on all sides) where the cellular automaton says wall, and leaves floor cells empty (`floor=-1`). `fill` is the starting wall fraction, `steps` the smoothing passes; `connect=true` (the default) walls off pockets that can't reach the main cave, so the whole floor is walkable.
4. **`levels.autotile`** replaces every rock cell with the tile that matches its neighbours (see [section 7](#7-auto-tiling-in-detail)).
5. **`levels.scatter`** puts props on 6% of the cells of the new `props` layer, `avoid_layer=walls` keeping them off the rock, and weights making tile 2 three times as common as 3 or 4.
6. **`levels.object_add`** creates the `entities` object layer on first use and puts a `spawn` point in the middle of cell (10, 16). `levels.find tile=-1` lists empty cells on `walls` if you need floor positions.
7. **`levels.collision from=walls`** turns every rock cell into merged rectangles of type `collision` on a `collision` object layer: 73 boxes instead of 746 cells, which is what a physics engine wants.
8. **`levels.save`** writes Tiled XML (tileset images referenced as `../art/rock.png`, relative to the map) and a flat PNG of the visible tile layers.

### From the shell

The same idea, shorter, without a script file:

```sh
wetwaresuite batch <<'EOF'
levels.new width=48 height=32 layer=walls -> m
levels.tileset_placeholder doc=$m path=art/rock.png style=autotile
levels.gen_caves doc=$m wall=15 floor=-1 seed=11
levels.autotile doc=$m base=0
levels.save doc=$m path=out/quick-cave.tmx
EOF
```

Open `out/quick-cave.tmx` in Tiled or your engine as is. Change `seed=` for a different cave.

### Asking Claude

> Make me a 48x32 cave level in levels with 16 px tiles: autotiled rock walls, some random props on the floor, a spawn point somewhere open, and collision boxes for the rock. Show me a preview, then save it as out/cave.tmx.

Claude calls, roughly: `levels_new`, `levels_tileset_placeholder` (style `autotile`, then `colors`), `levels_gen_caves` with `preview: true` (and again with another `seed` if the cave looks poor), `levels_autotile`, `levels_layer_add`, `levels_scatter`, `levels_find` with `tile: -1` to pick a floor cell, `levels_object_add`, `levels_collision`, `levels_preview` to check, and `levels_save`.

## 3. Task: a dungeon with rooms, entities and collision for your own engine

The goal: a roguelike floor of rooms and corridors, with every room recorded as an object, a spawn point and an exit, a chest with loot properties, collision as a 0/1 grid, exported as JSON and CSV that your own code reads, plus TMJ and LDtk for inspection.

### Batch script

```text
# dungeon.txt: rooms and corridors with entities, exported for a custom engine
levels.new width=56 height=40 tile_width=16 layer=floor -> d
levels.tileset_placeholder doc=$d path=art/dungeon.png tiles=8 columns=4 colors=#3b3b4f,#8a8aa3,#c9a66b,#5a3d2b name=dungeon
levels.layer_add doc=$d name=walls
levels.gen_rooms doc=$d layer=walls wall=1 floor=-1 min_room=5 max_room=11 corridor=2 objects_layer=entities seed=21
levels.fill_rect doc=$d layer=floor x=0 y=0 width=56 height=40 tile=0
levels.tile_property doc=$d tile=1 key=solid value=true
levels.collision doc=$d solid=property output=grid
levels.object_list doc=$d type=spawn
levels.object_list doc=$d type=exit
levels.object_add doc=$d layer=entities type=chest name=loot1 cell=16,8 properties="gold=50; locked=true"
levels.object_set doc=$d id=1 key=boss value=true
levels.save doc=$d path=out/dungeon.tmj
levels.save doc=$d path=out/dungeon.json
levels.save doc=$d path=out/dungeon.ldtk
levels.export_csv doc=$d output_dir=out/dungeon_csv
levels.save doc=$d path=out/dungeon.png scale=0.5
```

```sh
$ wetwaresuite batch dungeon.txt
...
generated 24 rooms on walls: 2,2 9x10; 3,31 5x5; 2,14 5x6; 2,22 10x5; 14,6 5x6; 22,5 7x6; ...
rooms: 24
filled 56x40 with dungeon:0 (2240 cells changed)
...
set dungeon:1 solid=true
built collision grid collision: 1160 solid cells
...
1 object
  #25 spawn [spawn] at 104,120 point on entities
...
1 object
  #26 exit [exit] at 680,568 point on entities
...
added object #27 loot1 [chest] at 256,128 size 16x16 on entities {gold=50; locked=true}
...
updated #1 room1 [room] at 32,32 size 144x160 on entities {boss=true}
...
saved out/dungeon.json (json)
...
wrote 3 CSV files: out/dungeon_csv/floor.csv, out/dungeon_csv/walls.csv, out/dungeon_csv/collision.csv
...
```

- **`levels.gen_rooms`** splits the map recursively (binary space partitioning), puts a room of `min_room`..`max_room` cells in each leaf, and joins sibling rooms centre to centre with L-shaped corridors `corridor` cells wide, so every room is reachable. It lists each room as `x,y wxh`. With `objects_layer`, every room becomes a `room` object (ids 1..24 here), with a `spawn` point in the first room and an `exit` in the last.
- The chest goes in room 5 (`14,6 5x6`), at cell 16,8. `properties` takes `key=value` pairs separated by `;`.
- **`levels.tile_property`** marks tile 1 (the wall) `solid=true`; **`levels.collision solid=property output=grid`** then writes a collision grid layer with 1 wherever any tile layer has a solid tile. Tile properties also travel in TMX/TMJ, so engines that read Tiled tile properties see them too.
- **`levels.object_set id=1 key=boss value=true`** tags the first room as the boss room.

The plain JSON keeps one row of gids per line and properties as JSON values:

```json
{
 "format": "wetwaresuite-level",
 "version": 1,
 "width": 56,
 "height": 40,
 "tileWidth": 16,
 "tileHeight": 16,
 "properties": {},
 "tilesets": [
  {
   "name": "dungeon",
   "firstGid": 1,
   "image": "../art/dungeon.png",
   ...
   "tiles": {
    "1": {
     "solid": true
    }
   }
  }
 ],
 "layers": [
  {
   "name": "floor",
   "type": "tiles",
   ...
   "data": [
    [1,1,1,1,1,1,1,1, ...],
```

The CSV files hold tile ids within the tileset, with `-1` for empty (what most engines' CSV loaders expect); `values=gid` writes raw gids instead. The collision grid is always plain 0/1:

```sh
$ head -3 out/dungeon_csv/collision.csv | cut -c1-60
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
```

### From the shell

```sh
wetwaresuite batch <<'EOF'
levels.new width=56 height=40 -> d
levels.gen_rooms doc=$d wall=0 floor=-1 objects_layer=entities seed=21
levels.export_csv doc=$d path=out/rooms.csv
levels.save doc=$d path=out/rooms.json
EOF
```

With no tileset, tile 0 is stored as gid 1 and drawn as a flat colour; the CSV still says `0` for walls and `-1` for floor.

### Asking Claude

> In levels, generate a 56x40 dungeon with rooms 5 to 11 cells across and 2-wide corridors (seed 21). Record the rooms as objects with a spawn and an exit, put a locked chest with 50 gold in a room near the spawn, mark the first room as the boss room, and give me collision as a 0/1 grid. Export JSON and per-layer CSV to out/.

Claude calls `levels_new`, `levels_tileset_placeholder`, `levels_layer_add`, `levels_gen_rooms` (with `objects_layer` and `preview: true`), `levels_object_list` with `type: "room"` to pick a room near the spawn, `levels_object_add` with `properties: "gold=50; locked=true"`, `levels_object_set`, `levels_tile_property`, `levels_collision` with `output: "grid"`, then `levels_save` and `levels_export_csv`.

## 4. Task: an overworld from noise, on a tileset packed by forge

The goal: you have loose 16x16 tile images (water, sand, grass, forest, rock, two flowers, a pebble) and want an overworld map: terrain by height, flowers on grass, pebbles on the beach, with water and rock blocking movement.

The loose tiles here are made by a two-line script (stand-ins for real art; skip this if you have tiles):

```text
# maketiles.txt: eight loose 16x16 tile images to start from (stand-ins for real art)
levels.new width=1 height=1 -> t
levels.tileset_placeholder doc=$t path=art/loose.png tiles=8 columns=8 colors=#2d6fb8,#e0cf8a,#5aa04a,#2f6b32,#8a8a8a,#e85d9c,#f2e65a,#a08a70
forge.sprite_split input=art/loose.png output_dir=tiles cell_width=16 cell_height=16 prefix=tile
```

```sh
$ wetwaresuite batch maketiles.txt
...
wrote 8 frames to tiles
frames: 8
skipped: 0
output_dir: tiles
first: tiles/tile_00.png
last: tiles/tile_07.png
```

### Batch script

```text
# overworld.txt: terrain from noise on a tileset packed by forge
# 1. loose tile images -> one sheet with 2 px padding (what an artist might hand you)
forge.sprite_sheet input_dir=tiles output=art/overworld.png layout=grid columns=4 padding=2
# 2. the map: 64x48 cells of 16 px
levels.new width=64 height=48 tile_width=16 layer=terrain -> w
levels.tileset_add doc=$w path=art/overworld.png margin=2 spacing=2 name=overworld
# water, sand, grass, forest, rock by height
levels.gen_noise doc=$w layer=terrain tiles=0,1,2,3,4 thresholds=0.32,0.40,0.68,0.85 scale=18 octaves=4 seed=2024
levels.layer_add doc=$w name=details
# flowers only on grass, a few rocks on sand
levels.scatter doc=$w layer=details tiles=5,6 on_layer=terrain on_tiles=2 density=0.05 seed=1
levels.scatter doc=$w layer=details tiles=7 on_layer=terrain on_tiles=1 density=0.08 seed=2
levels.count doc=$w layer=terrain
levels.tile_property doc=$w tile=0 key=solid value=true
levels.tile_property doc=$w tile=4 key=solid value=true
levels.collision doc=$w from=terrain solid=property
levels.save doc=$w path=out/overworld.tmx encoding=zlib
levels.save doc=$w path=out/overworld.png scale=0.5
```

```sh
$ wetwaresuite batch overworld.txt
packed 8 frames into 74x38 art/overworld.png + art/overworld.json (opened as img1)
...
added tileset overworld: 8 tiles (4 columns), gids 1..8
...
generated noise terrain on terrain: overworld:0 31% overworld:1 13% overworld:2 42% overworld:3 9% overworld:4 2%
...
scattered 73 tiles on details
...
scattered 33 tiles on details
...
3072 filled cells
terrain: 3072 filled, 0 empty
  overworld:2 x1314
  overworld:0 x981
  overworld:1 x409
  overworld:3 x283
  overworld:4 x85
...
built 53 collision rectangles covering 1066 solid cells on collision
...
saved out/overworld.tmx (tmx)
...
```

- **`forge.sprite_sheet layout=grid padding=2`** packs the tiles 4 to a row with 2 px around and between them: a 74x38 sheet. `levels.tileset_add margin=2 spacing=2` slices it back exactly. A sheet with `padding=0` needs no margin or spacing. Any tileset image from an artist or an asset pack works the same way.
- **`levels.gen_noise`** makes smooth fractal noise stretched to 0..1 and splits it at the `thresholds` into bands, lowest first: below 0.32 water (tile 0), then sand, grass, forest, and rock above 0.85. `scale` is the feature size in cells; more `octaves` add finer detail. The result line gives each band's share, so you can move a threshold to get more or less water.
- **`on_layer=terrain on_tiles=2`** restricts the flowers to grass cells; `on_tiles` uses the same tileset as `tiles`.
- **`encoding=zlib`** writes TMX tile data as base64 zlib, the smallest form Tiled reads (6.5 KB here); the default `csv` is easier to read and diff.

### From the shell

Try thresholds quickly without writing a script, then look at the PNG:

```sh
wetwaresuite batch <<'EOF'
levels.new width=64 height=48 -> w
levels.gen_noise doc=$w tiles=0,1,2 thresholds=0.4,0.7 scale=12 seed=7
levels.save doc=$w path=out/noise-test.png scale=0.25
EOF
```

### Asking Claude

> Use forge to pack the tiles in tiles/ into a 4-column sheet with 2 px padding, then in levels make a 64x48 overworld on it: water, sand, grass, forest and rock from noise (seed 2024), flowers on the grass, pebbles on the sand, and collision boxes for water and rock. Save it as out/overworld.tmx.

Claude calls `forge_sprite_sheet`, `levels_new`, `levels_tileset_add` (with `margin` and `spacing` matching the padding), `levels_gen_noise` with `preview: true`, adjusting `thresholds` if the bands look wrong, `levels_layer_add`, `levels_scatter` twice, `levels_tile_property` for tiles 0 and 4, `levels_collision` with `solid: "property"`, and `levels_save`.

## 5. Task: hand edits on an existing Tiled map

The goal: open the overworld from task 4, build a walled yard with a gate and a sand path, copy it, recover from a mistake, then grow the map by two cells on every side with a water border.

### Batch script

```text
# edit.txt: hand edits on an existing Tiled map
levels.open path=out/overworld.tmx -> w
# a walled 10x7 yard with a gate, on its own layer
levels.layer_add doc=$w name=build
levels.fill_rect doc=$w layer=build x=20 y=10 width=10 height=7 tile=4 outline=true
levels.erase doc=$w layer=build x=24 y=16 width=2
# a sand path from the gate down to row 30
levels.line doc=$w layer=build x1=24 y1=17 x2=24 y2=30 tile=1
levels.line doc=$w layer=build x1=25 y1=17 x2=25 y2=30 tile=1
# fill the yard's inside with sand (flood stops at the wall)
levels.flood_fill doc=$w layer=build x=22 y=12 tile=1
# copy the whole yard 15 cells to the right
levels.stamp doc=$w layer=build x=35 y=10 from_x=20 from_y=10 width=10 height=21
levels.cell doc=$w x=20 y=10
levels.find doc=$w layer=build tile=4 limit=4
# one mistake, undone
levels.fill_rect doc=$w layer=build x=0 y=0 width=64 height=48 tile=3
undo
# make room for a border of water on every side
levels.resize doc=$w width=68 height=52 anchor=center
levels.fill_rect doc=$w layer=terrain x=0 y=0 width=68 height=52 tile=0 outline=true
levels.fill_rect doc=$w layer=terrain x=1 y=1 width=66 height=50 tile=0 outline=true
levels.save doc=$w path=out/overworld2.tmj
levels.save doc=$w path=out/overworld2.png scale=0.5
```

```sh
$ wetwaresuite batch edit.txt
opened out/overworld.tmx as lvl1: 64x48 cells of 16x16 px, 2 tile layers, 1 object layer, 53 objects, 1 tileset, out/overworld.tmx
...
filled 10x7 outline with overworld:4 (30 cells changed)
...
erased 2 cells on build
...
drew a line of overworld:1 (14 cells changed)
...
filled 42 cells of empty with overworld:1
...
stamped a 10x21 block at 35,10 (98 cells changed)
...
cell 20,10
  terrain: overworld:3
  details: empty
  build: overworld:4 {solid=true}
...
56 cells of overworld:4
  build: 56
positions: 20,10 21,10 22,10 23,10 ...
...
filled 64x48 with overworld:3 (3072 cells changed)
...
undone
resized from 64x48 to 68x52 cells (content shifted 2,2)
...
filled 68x52 outline with overworld:0 (236 cells changed)
...
saved out/overworld2.tmj (tmj)
...
```

- The flood fill filled 42 cells: the 8x5 inside of the yard plus the two gate cells, where it stopped at the sand path.
- **`levels.stamp`** copies a region (here the yard and its path, 10x21 cells) to another place; empty source cells are skipped, so the terrain under them stays. `from_layer` and `from_doc` copy from another layer or another open level; `tileset_rect=column,row,width,height` stamps a block straight from the tileset image (a 2x3 tree, a house).
- **`levels.cell`** shows what is at a cell on every layer, with tile properties and the objects covering it.
- **`levels.resize anchor=center`** keeps the content in the middle: every layer and object moves by (2, 2). `offset_x`/`offset_y` shift by exact cell counts instead; cells pushed off the edge are lost.
- `undo` takes back exactly one command; a batch script can use it anywhere.

### From the shell

Look at what's in a map before editing it (one-shot, nothing is changed):

```sh
$ wetwaresuite levels.open path=out/cave.tmx
opened out/cave.tmx as lvl1: 48x32 cells of 16x16 px, 2 tile layers, 2 object layers, 74 objects, 2 tilesets, out/cave.tmx
lvl1
doc: lvl1
width: 48
height: 32
tile_width: 16
tile_height: 16
layers: 4
tilesets: 2
objects: 74
```

A quick edit-and-save pipes into `batch`:

```sh
wetwaresuite batch <<'EOF'
levels.open path=out/cave.tmx -> m
levels.paint doc=$m layer=props cells=10,16,11,16,12,16 tile=5 tileset=props
levels.save doc=$m path=out/cave.tmx
EOF
```

### Asking Claude

> Open out/overworld.tmx in levels. On a new layer called build, make a 10x7 stone-walled yard at cell 20,10 with a 2-wide gate in the bottom wall and a sand path from the gate down to row 30, fill the yard with sand, and copy it all 15 cells to the right. Then add a 2-cell water border around the whole map. Show me the result and save it as out/overworld2.tmj.

Claude calls `levels_open`, `levels_info` (to learn the tile ids), `levels_layer_add`, `levels_fill_rect` with `outline: true`, `levels_erase`, `levels_line` twice, `levels_flood_fill`, `levels_stamp`, `levels_preview` to check, `levels_resize` with `anchor: "center"`, `levels_fill_rect` twice, and `levels_save`. If a step goes wrong it calls `undo`.

## 6. Task: inspect and convert map files

**CLI.** `levels.open` is read-only, so it works as a one-shot inspector for any supported file:

```sh
$ wetwaresuite levels.open path=out/dungeon.ldtk
opened out/dungeon.ldtk as lvl1: 56x40 cells of 16x16 px, 3 tile layers, 1 object layer, 27 objects, 1 tileset, out/dungeon.ldtk
lvl1
doc: lvl1
width: 56
height: 40
...
```

The LDtk file came back with its floor and walls tile layers, the collision IntGrid (as a collision grid) and the entities with their fields.

**Batch.** Convert between formats; the output extension picks the format:

```sh
wetwaresuite batch <<'EOF'
levels.open path=out/cave.tmx -> m
levels.info doc=$m
levels.count doc=$m
levels.save doc=$m path=out/cave.tmj
levels.save doc=$m path=out/cave.ldtk
levels.save doc=$m path=out/cave_plain.json
levels.export_csv doc=$m output_dir=out/cave_csv
levels.save doc=$m path=out/cave_big.png scale=2
EOF
```

`levels.count` prints a histogram of tiles per layer; `levels.find tile=<id>` gives the count and positions of one tile. To bring a CSV grid from another tool into a map, `levels.import_csv path=walls.csv layer=walls` (it creates the layer if needed; `-1` or blank is empty).

**Claude.**

> What's in levels/world1.tmx? List the layers and objects, tell me which tiles are used most, and convert it to LDtk.

Claude calls `levels_open` with `preview: true`, `levels_info`, `levels_object_list`, `levels_count`, and `levels_save` with an `.ldtk` path, then reports any warnings `levels_save` printed (for example tiles from a second tileset on one layer, which LDtk can't hold).

## 7. Auto-tiling in detail

`levels.autotile` uses the simplest complete rule, 16 tiles chosen by a 4-bit mask of which of the four side neighbours are the same terrain:

| neighbour | bit |
| --- | --- |
| north (above) | 1 |
| east (right) | 2 |
| south (below) | 4 |
| west (left) | 8 |

A terrain cell becomes tile `base + mask`. So tile `base + 0` is an isolated block, `base + 15` the inside of a mass, `base + 6` (east + south) a top-left corner, `base + 5` (north + south) a vertical strip, and so on. Lay your 16 tiles out in that order, 4 per row is convenient; `levels.tileset_placeholder style=autotile` draws a template where each tile has a dark rim on the sides whose bit is off, which you can paint over.

Parameters:

- `layer` holds the terrain. By default any non-empty cell is terrain; `match=3,4` limits it to those tile ids (cells already holding one of the 16 auto-tiles count as terrain too, so you can run it again after more painting).
- `target` writes the result to another layer (created above the source if missing), leaving your rough painting intact.
- `x`, `y`, `width`, `height` limit the work to a region.
- `edges=solid` (default) counts cells beyond the map as terrain, so a wall touching the map edge has no rim there; `edges=empty` puts rims along the edge.

The usual workflow: paint or generate rough terrain with one tile (`levels.fill_rect`, `levels.gen_caves wall=15`), then `levels.autotile base=0`. The 4-bit rule ignores diagonal neighbours, so inside corners look square; for 47-tile "blob" sets use Tiled's terrain brushes on the exported map.

## 8. Objects, properties and collision

**Objects.** `levels.object_add` takes `type` (enemy, chest, door...), `name`, a position (`x`/`y` in px, or `cell=column,row`), a size (default one tile; points have none) and `properties="key=value; key2=value2"`. A point placed with `cell=` sits in the middle of the cell; a rect at its top-left. `levels.object_move` takes a new position, `dx`/`dy`, or `layer=` to move it to another object layer; `levels.object_set` changes name, type, size and shape and sets or removes one property (`key`, `value`, an empty value removes) or several (`properties`). `levels.object_list` shows ids, filtered by `type` or `layer`.

```sh
wetwaresuite batch <<'EOF'
levels.new width=20 height=15 -> m
levels.object_add doc=$m type=enemy name=bat cell=5,5 properties="hp=3; speed=1.5; flying=true"
levels.object_add doc=$m type=door x=144 y=96 width=16 height=32 properties="target=level2"
levels.object_move doc=$m id=1 dx=8
levels.object_set doc=$m id=2 key=locked value=true
levels.object_list doc=$m
EOF
```

```
...
2 objects
  #1 bat [enemy] at 88,80 size 16x16 on objects {flying=true; hp=3; speed=1.5}
  #2 [door] at 144,96 size 16x32 on objects {locked=true; target=level2}
...
```

**Property types** are inferred (`true`/`false` bool, `12` int, `1.5` float, `#ff0000` colour, anything else string) or set with `value_type=string|int|float|bool|color|file`. They are written with their types to TMX/TMJ (Tiled's custom properties), as JSON values in the plain JSON, and as entity fields in LDtk. Map and layer properties use `levels.property_set` (with `layer=` for a layer); tile properties `levels.tile_property`.

**Collision** comes from tile layers in two shapes:

- `output=objects` (default): merged rectangles of type `collision` on an object layer (runs along each row, merged downward when they line up). Load them as static bodies.
- `output=grid`: a collision grid tile layer of 0/1 cells (red overlay in previews, IntGrid in LDtk, plain 0/1 in CSV).

Which tiles are solid: `solid=any` (any tile on the `from` layers, default every non-collision tile layer), `solid=list tiles=0,4`, or `solid=property` (tiles whose property `property=` (default `solid`) is true). Running it again replaces the collision layer.

## 9. File formats

| format | open | save | notes |
| --- | --- | --- | --- |
| Tiled TMX (`.tmx`) | yes | yes | orthogonal maps; tile data CSV, base64, base64+zlib or base64+gzip (read); writes `csv`, `base64` or `zlib`; embedded or external `.tsx` tilesets (read); group layers are flattened; tile objects (`gid`) open as objects with a `gid` property |
| Tiled JSON (`.tmj`, or `.json` from Tiled) | yes | yes | same coverage as TMX; external `.tsj`/`.tsx` tilesets read |
| plain JSON (`.json`) | yes | yes | `format: wetwaresuite-level`: `data` is rows of gids (0 empty), tilesets with `firstGid`, objects with typed properties |
| LDtk-lite (`.ldtk`, or `.json` from LDtk) | yes (one level) | yes | Tiles and auto-layer tiles, IntGrid (as collision grids), entities with fields; one tileset per tile layer (others are left out with a warning); cells must be square |
| CSV (per layer) | `levels.import_csv` | `levels.export_csv` | tile ids within the tileset with -1 empty, or raw gids with `values=gid` |
| PNG/JPG/BMP/TGA | no | yes | flattened render of visible tile layers at `scale` |

`.json` files are sniffed when opened: Tiled's have `tiledversion`, LDtk's a `__header__`. Saving to `.json` always writes the plain format. Tileset image paths are stored relative to the saved file and resolved relative to the map when opening; if an image is missing the map still opens (with a warning) and its tiles draw as colours.

## 10. Command reference

`wetwaresuite help levels.<command>` prints the same information; over MCP the tool names use `_` (`levels_fill_rect`). Every edit command is undoable and takes `preview` (attach a PNG render to the result).

### Documents and files

#### `levels.new`

Create an empty tile map and return its id (e.g. lvl1): width x height cells of tile_width x tile_height px, with one tile layer. Then add a tileset (levels.tileset_add, or levels.tileset_placeholder to draw one) and paint, fill or generate. Without a tileset, tile n draws as a flat colour.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `width` | integer |  | 32 | map width in cells |
| `height` | integer |  | 24 | map height in cells |
| `tile_width` | integer |  | 16 | tile width in px |
| `tile_height` | integer |  |  | tile height in px (default: tile_width) |
| `layer` | string |  | ground | name of the first tile layer |
| `background` | string |  | transparent | background colour, e.g. #203040 |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.open`

Open a map: Tiled .tmx (CSV, base64, zlib or gzip data; embedded or external .tsx tilesets) or .tmj, the plain .json format levels.save writes, or an LDtk .ldtk/.json project (one level; IntGrid layers become collision grids, entities become objects). Tileset images load from paths relative to the map.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `path` | string | yes |  | map file |
| `level` | string |  |  | LDtk: level identifier (default: the first level) |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.save`

Save a map; the extension picks the format: .tmx (Tiled XML), .tmj (Tiled JSON), .json (plain JSON: rows of gids, tilesets, objects with properties; reopen with levels.open), .ldtk (LDtk-lite project JSON), or .png/.jpg/.bmp/.tga for a flattened render of the visible tile layers. Tileset image paths are written relative to the saved file. For CSV use levels.export_csv.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `path` | string | yes |  | output file |
| `format` | string |  | auto | override the extension [auto\|tmx\|tmj\|json\|ldtk\|png] |
| `encoding` | string |  | csv | TMX/TMJ tile data: csv (readable), base64, or zlib (smallest) [csv\|base64\|zlib] |
| `scale` | number |  | 1 | image exports: zoom factor |

#### `levels.close`

Close an open level (undo reopens it).

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |

#### `levels.info`

Describe a map: size, tilesets (tile ranges and images), layers bottom to top with how many cells or objects each holds, and map properties.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.preview`

Render the map to a PNG attachment (nothing is written): tile layers bottom to top, collision grids as a red overlay, objects as coloured boxes (points as dots), optionally a grid. Use it to look at the map.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layers` | string |  |  | comma-separated layer names or indexes to draw (default: all visible) |
| `scale` | number |  |  | zoom factor (default: fit small maps to about 512 px) |
| `grid` | boolean |  | false | draw cell lines |
| `objects` | boolean |  | true | draw objects |
| `collision` | boolean |  | true | draw collision grids |
| `max_size` | integer |  | 1024 | longest side of the PNG in px |

#### `levels.import_csv`

Read a CSV grid (one row per line, comma-separated) into a tile layer, created if missing. values=local reads tile ids of the first tileset (or tileset=) with -1 (or blank) as empty, as Tiled's CSV export and most engines write; values=gid reads raw gids with 0 as empty.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `path` | string | yes |  | CSV file |
| `layer` | string |  |  | target tile layer (default: the file name) |
| `values` | string |  | local | what the numbers are [local\|gid] |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.export_csv`

Write tile layers as CSV grids: one layer to path=, or every tile layer to output_dir/<layer>.csv. values=local writes tile ids within each tileset with -1 for empty; values=gid writes raw gids, 0 empty.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | with path=: the layer to write (default: the top tile layer) |
| `path` | string |  |  | one CSV file |
| `output_dir` | string |  |  | folder for one CSV per tile layer |
| `values` | string |  | local | what to write [local\|gid] |

### Tilesets

#### `levels.tileset_add`

Slice an image (png, jpg, bmp, tga, gif, psd...) into tiles and add it as a tileset. Tiles are numbered from 0, left to right, top to bottom; margin is the border around the sheet and spacing the gap between tiles (as in Tiled). Sprite sheets from forge.sprite_sheet layout=grid padding=0 work directly.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `path` | string | yes |  | tileset image |
| `tile_width` | integer |  |  | tile width in px (default: the map's) |
| `tile_height` | integer |  |  | tile height in px (default: the map's) |
| `margin` | integer |  | 0 | px around the edge of the image |
| `spacing` | integer |  | 0 | px between tiles |
| `name` | string |  |  | tileset name (default: the file name) |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.tileset_placeholder`

Draw a placeholder tileset image, write it to `path` and add it to the map: style=colors gives one distinct colour per tile (with its id's bits as dots along the top); style=autotile gives the 16 tiles of the 4-bit auto-tile rule (tile n has a dark rim on each side whose neighbour bit is off: N=1 E=2 S=4 W=8), ready for levels.autotile base=<its first tile>. Good for prototyping before the art exists.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `path` | string | yes |  | PNG to write |
| `style` | string |  | colors | what to draw [colors\|autotile] |
| `tiles` | integer |  | 16 | number of tiles (autotile: 16) |
| `columns` | integer |  | 8, autotile 4 | tiles per row |
| `colors` | string |  |  | comma-separated colours for the tiles (autotile: the body colour) |
| `name` | string |  |  | tileset name (default: the file name) |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.tile_property`

Set (or with an empty value, remove) a custom property on one tile of a tileset, e.g. solid=true for levels.collision solid=property, or damage=5. Saved in TMX/TMJ/JSON like Tiled's tile properties.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `tile` | integer | yes |  | tile id within the tileset |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `key` | string | yes |  | property name |
| `value` | string |  |  | value; empty removes the property |
| `value_type` | string |  | auto | property type (auto: from the value) [auto\|string\|int\|float\|bool\|color\|file] |

### Layers and properties

#### `levels.layer_add`

Add a tile layer or an object layer (for entities: spawns, pickups, triggers). New layers go on top unless index is given (0 = bottom).

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `name` | string | yes |  | layer name |
| `kind` | string |  | tiles | what the layer holds [tiles\|objects] |
| `index` | integer |  |  | position, 0 = bottom (default: top) |
| `collision` | boolean |  | false | tile layers: mark as a collision grid (drawn as an overlay) |

#### `levels.layer_remove`

Delete a layer and everything on it.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string | yes |  | layer name or index |

#### `levels.layer_rename`

Rename a layer.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string | yes |  | layer name or index |
| `name` | string | yes |  | new name |

#### `levels.layer_move`

Move a layer in the stack: to an index (0 = bottom) or up, down, top or bottom. Layers draw bottom first.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string | yes |  | layer name or index |
| `index` | integer |  |  | new position, 0 = bottom |
| `direction` | string |  |  | instead of index [up\|down\|top\|bottom] |

#### `levels.layer_set`

Change a layer's visibility, opacity or collision flag.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string | yes |  | layer name or index |
| `visible` | boolean |  |  | shown in renders and marked visible in exports |
| `opacity` | number |  |  | 0..1 |
| `collision` | boolean |  |  | tile layers: treat as a collision grid |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.property_set`

Set (or with an empty value, remove) a custom property on the map, or on a layer when layer= is given. Object properties use levels.object_set; tile properties levels.tile_property.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `key` | string | yes |  | property name |
| `value` | string |  |  | value; empty removes it |
| `layer` | string |  |  | layer name or index (default: the map itself) |
| `value_type` | string |  | auto | property type (auto: from the value) [auto\|string\|int\|float\|bool\|color\|file] |

### Painting

#### `levels.paint`

Put a tile in one cell (x = column, y = row, from the top-left), or in many: cells=x1,y1,x2,y2,...

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer name or index (0 = bottom); default: the top tile layer |
| `x` | integer |  |  | column |
| `y` | integer |  |  | row |
| `cells` | array |  |  | x,y pairs instead of x and y |
| `tile` | integer | yes |  | tile id within the tileset (0 = its first tile, -1 = empty) |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `flip_h` | boolean |  | false | mirror the tile horizontally |
| `flip_v` | boolean |  | false | mirror the tile vertically |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.fill_rect`

Fill a rectangle of cells with a tile (outline=true for just its border, e.g. walls).

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer name or index (0 = bottom); default: the top tile layer |
| `x` | integer | yes |  | left column |
| `y` | integer | yes |  | top row |
| `width` | integer | yes |  | cells across |
| `height` | integer | yes |  | cells down |
| `tile` | integer | yes |  | tile id within the tileset (0 = its first tile, -1 = empty) |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `outline` | boolean |  | false | only the border |
| `flip_h` | boolean |  | false | mirror the tile horizontally |
| `flip_v` | boolean |  | false | mirror the tile vertically |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.flood_fill`

Bucket fill: replace the connected area of cells matching the tile at (x, y) (empty counts too) with a tile.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer name or index (0 = bottom); default: the top tile layer |
| `x` | integer | yes |  | column |
| `y` | integer | yes |  | row |
| `tile` | integer | yes |  | tile id within the tileset (0 = its first tile, -1 = empty) |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `diagonal` | boolean |  | false | spread across corners too |
| `flip_h` | boolean |  | false | mirror the tile horizontally |
| `flip_v` | boolean |  | false | mirror the tile vertically |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.line`

Draw a straight line of tiles from (x1, y1) to (x2, y2), both ends included.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer name or index (0 = bottom); default: the top tile layer |
| `x1` | integer | yes |  | start column |
| `y1` | integer | yes |  | start row |
| `x2` | integer | yes |  | end column |
| `y2` | integer | yes |  | end row |
| `tile` | integer | yes |  | tile id within the tileset (0 = its first tile, -1 = empty) |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `flip_h` | boolean |  | false | mirror the tile horizontally |
| `flip_v` | boolean |  | false | mirror the tile vertically |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.erase`

Empty one cell, a rectangle (width/height), or the whole layer (all=true).

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer name or index (0 = bottom); default: the top tile layer |
| `x` | integer |  |  | column |
| `y` | integer |  |  | row |
| `width` | integer |  | 1 | cells across |
| `height` | integer |  | 1 | cells down |
| `all` | boolean |  | false | clear the whole layer |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.stamp`

Copy a block of tiles to (x, y): either a region of a layer (from_x, from_y, width, height; from_layer and from_doc default to this layer and level) or a block straight from the tileset image (tileset_rect=column,row,width,height in tiles, e.g. a 2x3 tree). Empty source cells are skipped unless skip_empty=false.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer to stamp onto (default: the top tile layer) |
| `x` | integer | yes |  | destination column |
| `y` | integer | yes |  | destination row |
| `from_doc` | string |  |  | level to copy from (default: this one) |
| `from_layer` | string |  |  | layer to copy from (default: the destination layer) |
| `from_x` | integer |  |  | source left column |
| `from_y` | integer |  |  | source top row |
| `width` | integer |  |  | block width in cells |
| `height` | integer |  |  | block height in cells |
| `tileset_rect` | array |  |  | column,row,width,height of a block of tiles in the tileset image |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `skip_empty` | boolean |  | true | leave cells under empty source cells alone |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

### Auto-tiling, scatter and generators

#### `levels.autotile`

Pick the right edge/corner tile for every terrain cell with the 16-tile (4-bit) rule: mask = N 1 + E 2 + S 4 + W 8 for each side whose neighbour is terrain too, and the cell becomes tile base + mask. Lay your 16 tiles out in that order (levels.tileset_placeholder style=autotile draws a template). Paint rough terrain with any tile, then run this; the result can go to another layer (target=).

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer holding the terrain (default: the top tile layer) |
| `target` | string |  |  | layer to write the auto-tiles to (default: the same layer) |
| `base` | integer |  | 0 | tile id of the first of the 16 auto-tiles (mask 0) |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `match` | array |  |  | tile ids that count as terrain (default: any non-empty cell) |
| `edges` | string |  | solid | whether cells beyond the map count as terrain [solid\|empty] |
| `x` | integer |  | 0 | region left column |
| `y` | integer |  | 0 | region top row |
| `width` | integer |  |  | region width in cells (default: to the right edge) |
| `height` | integer |  |  | region height in cells (default: to the bottom edge) |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.scatter`

Sprinkle tiles at random (flowers on grass, rocks, treasure): by density or an exact count, optionally weighted, only on empty cells, only on top of some tiles of another layer, and away from another layer. Deterministic for a seed.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer name or index (0 = bottom); default: the top tile layer |
| `tiles` | array | yes |  | tile ids to scatter, e.g. 4,5,6 |
| `weights` | array |  | equal | relative chance of each tile |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `density` | number |  | 0.1 | chance per allowed cell, 0..1 |
| `count` | integer |  |  | place exactly this many instead of density |
| `seed` | integer |  | 1 | random seed: same seed, same result |
| `only_empty` | boolean |  | true | only cells that are empty on this layer |
| `on_layer` | string |  |  | only cells where this layer has a tile... |
| `on_tiles` | array |  |  | ...one of these tile ids (default: any) |
| `avoid_layer` | string |  |  | skip cells where this layer has a tile (e.g. walls) |
| `x` | integer |  | 0 | region left column |
| `y` | integer |  | 0 | region top row |
| `width` | integer |  |  | region width in cells (default: to the right edge) |
| `height` | integer |  |  | region height in cells (default: to the bottom edge) |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.gen_caves`

Fill a layer with organic caves from cellular automata: random walls (fill), then smoothing steps where a cell becomes wall when most neighbours are. connect=true walls off pockets that can't reach the main cave. Writes wall and floor tiles (floor=-1 leaves floor cells empty); follow with levels.autotile for edges. Returns the floor cell count.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer name or index (0 = bottom); default: the top tile layer |
| `wall` | integer | yes |  | tile id for walls |
| `floor` | integer |  |  | tile id for floor (-1 = empty) |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `fill` | number |  | 0.45 | starting wall fraction 0..1 |
| `steps` | integer |  | 5 | smoothing passes |
| `connect` | boolean |  | true | keep only the largest connected cave |
| `seed` | integer |  | 1 | random seed: same seed, same map |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.gen_rooms`

Dungeon rooms and corridors by binary space partitioning: the map is split recursively, each leaf gets a room, and sibling rooms are joined by L-shaped corridors so every room is reachable. With objects_layer, each room is added as a 'room' object plus a 'spawn' point in the first room and an 'exit' in the last.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer name or index (0 = bottom); default: the top tile layer |
| `wall` | integer | yes |  | tile id for walls (-1 = empty) |
| `floor` | integer | yes |  | tile id for floor (-1 = empty) |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `min_room` | integer |  | 4 | smallest room side in cells |
| `max_room` | integer |  | 10 | largest room side in cells |
| `corridor` | integer |  | 1 | corridor width in cells |
| `padding` | integer |  | 1 | minimum wall between neighbouring rooms' leaves |
| `objects_layer` | string |  |  | object layer to add room/spawn/exit objects to (created if missing) |
| `seed` | integer |  | 1 | random seed: same seed, same map |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.gen_noise`

Overworld terrain from fractal value noise (0..1, stretched to the full range): thresholds split it into bands and each band gets a tile, e.g. thresholds=0.35,0.45,0.75 tiles=water,sand,grass,mountain (as ids). tiles needs one more entry than thresholds; -1 leaves a band empty.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | tile layer name or index (0 = bottom); default: the top tile layer |
| `tiles` | array | yes |  | tile id per band, lowest first |
| `thresholds` | array | yes |  | band boundaries in 0..1, ascending |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `scale` | number |  | 16 | feature size in cells (bigger = broader) |
| `octaves` | integer |  | 4 | detail layers |
| `persistence` | number |  | 0.5 | how much each finer octave adds, 0..1 |
| `seed` | integer |  | 1 | random seed: same seed, same map |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

### Objects

#### `levels.object_add`

Add an entity to an object layer: a rect (trigger zones, rooms), ellipse, or point (spawns). Position and size are in px from the map's top-left, or give cell=column,row. Custom properties as properties="hp=10; loot=gold" (types inferred). The layer is created if it doesn't exist.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | object layer (default: the top one, or a new 'objects') |
| `name` | string |  |  | object name |
| `type` | string |  |  | object type/class, e.g. enemy, chest, spawn |
| `x` | number |  |  | left in px |
| `y` | number |  |  | top in px |
| `cell` | array |  |  | column,row: place on a cell instead of x,y (size defaults to one tile) |
| `width` | number |  |  | px (default: one tile; 0 for points) |
| `height` | number |  |  | px (default: one tile; 0 for points) |
| `shape` | string |  | rect | object shape [rect\|ellipse\|point] |
| `properties` | string |  |  | custom properties: key=value; key2=value2 |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.object_move`

Move an object to x,y (px) or cell=column,row, or by dx,dy px.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `id` | integer | yes |  | object id (levels.object_list shows them) |
| `x` | number |  |  | new left in px |
| `y` | number |  |  | new top in px |
| `cell` | array |  |  | column,row: place on a cell instead of x,y (size defaults to one tile) |
| `dx` | number |  |  | move right by px |
| `dy` | number |  |  | move down by px |
| `layer` | string |  |  | also move it to this object layer |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.object_set`

Change an object's name, type, size or shape, and/or set one custom property (key + value; an empty value removes it). properties="a=1; b=2" sets several at once.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `id` | integer | yes |  | object id (levels.object_list shows them) |
| `name` | string |  |  | new name |
| `type` | string |  |  | new type |
| `width` | number |  |  | px |
| `height` | number |  |  | px |
| `shape` | string |  |  | object shape [rect\|ellipse\|point] |
| `key` | string |  |  | custom property name |
| `value` | string |  |  | its value (empty removes it) |
| `value_type` | string |  | auto | property type (auto: from the value) [auto\|string\|int\|float\|bool\|color\|file] |
| `properties` | string |  |  | several properties: key=value; key2=value2 |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.object_remove`

Delete an object.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `id` | integer | yes |  | object id (levels.object_list shows them) |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.object_list`

List objects with their ids, positions, sizes and properties, optionally of one type or layer.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | only this object layer |
| `type` | string |  |  | only objects of this type |

### Collision, queries and resizing

#### `levels.collision`

Build collision from tile layers: a cell is solid when a source layer has a solid tile there; solid=any (any tile), list (the ids in tiles=), or property (tiles with a property, default solid=true, set by levels.tile_property). output=objects writes merged rectangles (type 'collision') to an object layer, which engines load as static bodies; output=grid writes a 0/1 collision grid tile layer. The target layer is replaced.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `from` | string |  |  | comma-separated source layers (default: every non-collision tile layer) |
| `solid` | string |  | any | which tiles are solid [any\|list\|property] |
| `tiles` | array |  |  | solid=list: the solid tile ids |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `property` | string |  | solid | solid=property: the property that must be true |
| `output` | string |  | objects | what to write [objects\|grid] |
| `name` | string |  | collision | target layer name |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

#### `levels.find`

Find the cells holding a tile: count and positions (x,y), on one layer or all tile layers.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `tile` | integer | yes |  | tile id within the tileset (-1 = empty cells) |
| `tileset` | string |  |  | tileset name or index the tile ids refer to (default: the first tileset) |
| `layer` | string |  |  | only this layer (default: all tile layers) |
| `limit` | integer |  | 100 | most positions to list |

#### `levels.count`

How many cells use each tile, per tile layer (a tile histogram).

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `layer` | string |  |  | only this layer (default: all tile layers) |

#### `levels.cell`

What is at a cell: the tile on every tile layer (with tile properties) and the objects that cover it.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `x` | integer | yes |  | column |
| `y` | integer | yes |  | row |

#### `levels.resize`

Change the map size in cells. anchor says where the old content sits in the new map (top-left keeps it at 0,0; center centres it); offset_x/offset_y shift it by whole cells instead. Objects move with the tiles; cells pushed off the edge are lost.

| param | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `doc` | string | yes |  | id of an open level, e.g. lvl1 |
| `width` | integer | yes |  | new width in cells |
| `height` | integer | yes |  | new height in cells |
| `anchor` | string |  | top-left | where the old content goes [top-left\|top\|top-right\|left\|center\|right\|bottom-left\|bottom\|bottom-right] |
| `offset_x` | integer |  |  | shift old content right by cells (overrides anchor) |
| `offset_y` | integer |  |  | shift old content down by cells (overrides anchor) |
| `preview` | boolean |  | false | attach a PNG render of the map to the result |

## 11. Limitations

- **Orthogonal, finite maps only.** Isometric, hexagonal and staggered maps, and Tiled's infinite (chunked) maps, are refused with a message; turn off Map > Infinite in Tiled before saving.
- **No GUI yet.** You see the map through `preview=true`, `levels.preview` and PNG exports; the GUI that comes later will call these same commands.
- **TMX/TMJ coverage.** Image layers, Wang/terrain sets, animations, object templates, layer offsets and parallax are not kept (skipped on open); polygon, polyline and text objects open as plain points or boxes, without their shape or text. Group layers are flattened into their children. Zstandard-compressed tile data can't be read. Tile objects keep their gid as a `gid` property but render as boxes. Image-collection tilesets (one image per tile) open without pixels, so their tiles draw as colours.
- **LDtk-lite** writes and reads a single level of an LDtk project with the fields engines' loaders use (layer instances with grid tiles, IntGrid values, entities with fields). The LDtk editor may want more definitions than are written; treat it as an export for runtimes, not a round trip with the editor. Multi-world projects and levels in separate files aren't read. One tileset per tile layer and square cells.
- **Auto-tiling** is the 16-tile, 4-neighbour rule; there is no 47-tile blob rule and no diagonal corner handling.
- **Generators** are simple: caves are cellular automata, dungeons are BSP rooms with L-shaped corridors (no doors or room types), terrain is value noise. They are fast and deterministic for a seed, not a full procedural design system.
- **Rendering** is nearest-pixel tiles at integer positions with layer opacity; no blend modes, tints or parallax. Tiles taller than the grid are anchored bottom-left like Tiled.
- **Size.** Maps are capped at 4096x4096 cells for `levels.new`/`levels.resize` and 16 million cells when opening. Undo snapshots the whole map per step, which is fine for typical levels but uses memory on very large ones.
- **Objects** have no rotation; rotated objects from Tiled open unrotated.
