# Offline editions

An offline edition is one folder (or archive) with every wetwaresuite app, all the tutorials and as much free content as fits: fonts, starter files, materials, sounds and world maps. Put it on a USB stick, a shared drive or a laptop that never goes online, and install from it with no internet at all.

Editions come in sizes that match USB sticks and SD cards: 1, 2, 4, 8, 16, 32, 64, 128 and 256 GB. Bigger editions hold more content; the apps are the same in all of them.

## What each edition holds

| edition | about | what's in it |
| --- | --- | --- |
| 1 GB | 0.6 GB | every app, tutorials, starter files, world map (countries and big cities), sounds |
| 2 GB | 1.7 GB | the above + about 1,500 font families |
| 4 GB | 1.7 GB | same as 2 GB (the next pack, materials, needs more room) |
| 8 GB | 6.7 GB | + about 2,000 materials at 1K (wood, stone, metal, fabric...) |
| 16 GB | 12.4 GB | world map down to towns and main roads (replaces the small map) |
| 32 GB | 25.4 GB | materials at 2K (replace the 1K ones) |
| 64 GB | 49.4 GB | world map down to streets in most places |
| 128 GB | 101.4 GB | materials at 4K |
| 256 GB | 196.4 GB | the full planet map, down to buildings and footpaths |

Sizes are estimates, in the same decimal gigabytes printed on USB sticks; an edition fills at most 95% of its size so there's room for the file system. `hub editions` prints this table from the pack list built into the hub, so it's always current.

## The content packs

| pack | what it is | size | licence | from | used by |
| --- | --- | --- | --- | --- | --- |
| apps (one per app) | the programs | 1-7 MB each | GPL-3.0 | your build | all |
| tutorials | these guides | 20 MB | GPL-3.0 | this repo | all |
| fonts | the Google Fonts library | 1.1 GB | OFL / Apache-2.0 / Ubuntu Font Licence | github.com/google/fonts | draw, office |
| samples | starter files: a texture, a logo, a model, a part, a song, a cave level, a world map project | 50 MB | CC0 | made by the apps while bundling (no download) | all |
| maps-world-low | world map tiles, zoom 0-6 | 250 MB | ODbL (OpenStreetMap contributors, Protomaps) | build.protomaps.com | maps |
| sounds | sound effects and instrument samples | 200 MB | CC0 | kenney.nl (downloaded by hand, see below) | sound |
| textures-1k / 2k / 4k | about 2,000 materials | 5 / 18 / 70 GB | CC0 | ambientcg.com | model, game |
| maps-world-mid / high / planet | world map tiles, zoom 0-10 / 0-12 / 0-15 | 6 / 30 / 125 GB | ODbL | build.protomaps.com | maps |

Everything is free to copy and pass on. Every edition has a `LICENSES.txt` listing each pack's licence and source. The map data needs credit: "© OpenStreetMap contributors".

Map tiles come as single [PMTiles](https://github.com/protomaps/PMTiles) files (`packs/maps-world-*/world.pmtiles`; MBTiles is the other accepted format), read straight from disk by the maps app with no tile server.

## Install from an edition (no internet)

1. Plug in the stick, or unpack the archive.
2. Run the hub that's inside it:

   ```sh
   /media/usb/wetwaresuite-32gb/bin/hub install --from /media/usb/wetwaresuite-32gb
   ```

   On Windows: `E:\wetwaresuite-32gb\bin\hub.exe install --from E:\wetwaresuite-32gb`

   That installs every app, the content packs and the tutorials into your personal wetwaresuite folder (see [apps](apps.md)).

3. Or install only some apps; the packs those apps use come along:

   ```sh
   hub install maps --from /media/usb/wetwaresuite-32gb             # the maps app + map tiles + samples
   hub install image model --from wetwaresuite-32gb.zip --packs none # just the programs
   ```

`--from` takes a folder or an archive (`.zip`, `.tar`, `.tar.gz`). Nothing is ever downloaded while installing. You can also run the apps straight from the edition's `bin/` folder without installing.

## Make an edition

Making an edition is a build step: it copies the apps you built and may download the optional packs. Do it once on a machine with internet, then copy the folder anywhere.

1. Build the apps and the share tool:

   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
   (cd share && cargo build --release)
   (cd hub && cargo build --release)
   ```

2. See what you'd get, without downloading anything:

   ```sh
   hub/target/release/hub bundle --size 32 --out wetwaresuite-32gb --dry-run
   ```

3. Make it:

   ```sh
   hub/target/release/hub bundle --size 32 --out wetwaresuite-32gb \
       --bin-dir build/bin --bin-dir build/apps/wetwaresuite --bin-dir share/target/release --docs docs
   ```

Options:

- `--size auto` picks the smallest edition that holds everything you asked for. It starts at 32 GB, doubles while the content doesn't fit and halves while half would still do.
- `--packs fonts,samples` limits the optional packs; the apps and tutorials are always in. `--size auto --packs fonts` gives a 2 GB edition.
- `--cache <folder>` uses packs you already downloaded: `<cache>/<pack id>/` is copied instead of downloading. Keep one cache and making several editions is quick.

How packs are fetched:

- **fonts**: one archive from GitHub, unpacked.
- **samples**: made on the spot by running the apps' own scripts. No download.
- **textures**: the ambientCG download list is read and each material at the chosen resolution is downloaded.
- **maps**: cut from the Protomaps planet build with the [`pmtiles`](https://github.com/protomaps/go-pmtiles/releases) tool (`pmtiles extract`), which only downloads the zoom levels needed.
- **sounds**: there's no single download link, so get the CC0 audio packs from kenney.nl yourself, unpack them into `<cache>/sounds/`, and pass `--cache`. Without it the edition is made without sounds.

If an optional pack fails to download, the edition is still made without it, and `bundle.txt` lists only what's really inside.

## What's in the folder

```
wetwaresuite-32gb/
  bin/            hub, wetwaresuite, and every wetwaresuite-<app>
  docs/           the tutorials
  packs/<id>/     fonts, samples, textures, map tiles...
  bundle.txt      the edition size and one line per pack
  LICENSES.txt    each pack's licence and source
  README.txt      how to install from it
```
