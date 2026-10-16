//! Content packs and offline editions.
//!
//! An offline edition is a folder (for a USB stick or a download) holding the
//! apps, the tutorials and as much free content as fits a power-of-two size:
//! 1, 2, 4, 8, 16, 32, 64, 128, 256 GB. Making a bundle may download (it is a
//! build step); installing from one never does.
//!
//! Sizes are decimal megabytes (1 GB = 1000 MB), like USB stick labels. An
//! edition only fills [`USABLE`] of its nominal size, leaving room for the file
//! system and for packs coming out a little bigger than estimated.

use crate::catalog::{App, APPS};

/// Fraction of an edition's nominal size that packs may fill.
pub const USABLE: f64 = 0.95;

/// Where the starting guess for `--size auto` sits; it doubles or halves from here.
pub const START_GB: u64 = 32;

/// How a pack's files are fetched when a bundle is made.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Fetch {
    /// An app binary, copied from a local build (`--bin-dir`).
    AppBinary,
    /// The tutorials, copied from the source tree (`--docs`).
    Docs,
    /// Made on this machine by running the apps' own batch scripts: no download.
    Generate,
    /// One archive at `url`, downloaded with curl.
    Url,
    /// `url` is a CSV/text list of files; every link on a line containing `filter` is downloaded.
    List { filter: &'static str },
    /// A PMTiles extract of the planet build at `url`, cut with the `pmtiles` command-line tool.
    Pmtiles { maxzoom: u8 },
    /// No stable direct link: put the files in `<cache>/<id>/` yourself and the bundler copies them.
    Manual,
}

#[derive(Debug, Clone, Copy)]
pub struct Pack {
    pub id: &'static str,
    pub summary: &'static str,
    pub approx_mb: u64,
    /// Must be free to redistribute: CC0, OFL, Apache-2.0, ODbL (with attribution), GPL.
    pub license: &'static str,
    pub source: &'static str,
    /// Lower comes first when an edition is filled. Required packs ignore it.
    pub priority: u32,
    /// The app that uses it ("" = any).
    pub app: &'static str,
    pub required: bool,
    /// Smaller tiers of the same content that this pack makes unnecessary.
    pub replaces: &'static [&'static str],
    pub fetch: Fetch,
}

#[allow(clippy::too_many_arguments)] // a table row: every field is named at the call
const fn optional(
    id: &'static str,
    summary: &'static str,
    approx_mb: u64,
    license: &'static str,
    source: &'static str,
    priority: u32,
    app: &'static str,
    replaces: &'static [&'static str],
    fetch: Fetch,
) -> Pack {
    Pack { id, summary, approx_mb, license, source, priority, app, required: false, replaces, fetch }
}

const PROTOMAPS: &str = "https://build.protomaps.com/20251001.pmtiles";
const AMBIENTCG: &str = "https://ambientcg.com/api/v2/downloads_csv";
const MAPS_LICENSE: &str = "ODbL-1.0 (map data (c) OpenStreetMap contributors; tiles by Protomaps)";

/// Optional packs, in priority order. Map tiles and textures come in tiers:
/// each bigger tier replaces the smaller ones.
pub const OPTIONAL: &[Pack] = &[
    optional(
        "fonts",
        "about 1,500 font families for draw, office and type (the Google Fonts library)",
        1100,
        "OFL-1.1 / Apache-2.0 / Ubuntu Font Licence (each family ships its licence)",
        "https://github.com/google/fonts/archive/refs/heads/main.zip",
        10,
        "draw",
        &[],
        Fetch::Url,
    ),
    optional(
        "samples",
        "starter files for every app (pictures, songs, parts, levels, maps) made by the apps themselves",
        50,
        "CC0-1.0",
        "made locally by `hub bundle` with each app's batch scripts (no download)",
        20,
        "",
        &[],
        Fetch::Generate,
    ),
    optional(
        "maps-world-low",
        "offline world map down to country and big-city level (zoom 0-6)",
        250,
        MAPS_LICENSE,
        PROTOMAPS,
        30,
        "maps",
        &[],
        Fetch::Pmtiles { maxzoom: 6 },
    ),
    optional(
        "sounds",
        "sound effects and instrument samples for sound (beats and audio)",
        200,
        "CC0-1.0",
        "https://kenney.nl/assets/category:Audio (download the packs into <cache>/sounds/)",
        40,
        "sound",
        &[],
        Fetch::Manual,
    ),
    optional(
        "textures-1k",
        "about 2,000 materials (wood, stone, metal, fabric...) at 1K for model and game",
        5000,
        "CC0-1.0",
        AMBIENTCG,
        50,
        "model",
        &[],
        Fetch::List { filter: "1K-JPG" },
    ),
    optional(
        "maps-world-mid",
        "offline world map down to towns and main roads (zoom 0-10)",
        6000,
        MAPS_LICENSE,
        PROTOMAPS,
        60,
        "maps",
        &["maps-world-low"],
        Fetch::Pmtiles { maxzoom: 10 },
    ),
    optional(
        "textures-2k",
        "the same materials at 2K",
        18000,
        "CC0-1.0",
        AMBIENTCG,
        70,
        "model",
        &["textures-1k"],
        Fetch::List { filter: "2K-JPG" },
    ),
    optional(
        "maps-world-high",
        "offline world map down to streets in most places (zoom 0-12)",
        30000,
        MAPS_LICENSE,
        PROTOMAPS,
        80,
        "maps",
        &["maps-world-low", "maps-world-mid"],
        Fetch::Pmtiles { maxzoom: 12 },
    ),
    optional(
        "textures-4k",
        "the same materials at 4K",
        70000,
        "CC0-1.0",
        AMBIENTCG,
        90,
        "model",
        &["textures-1k", "textures-2k"],
        Fetch::List { filter: "4K-JPG" },
    ),
    optional(
        "maps-planet",
        "the full planet down to buildings and footpaths (zoom 0-15)",
        125000,
        MAPS_LICENSE,
        PROTOMAPS,
        100,
        "maps",
        &["maps-world-low", "maps-world-mid", "maps-world-high"],
        Fetch::Pmtiles { maxzoom: 15 },
    ),
];

/// The tutorials: always in a bundle.
pub const TUTORIALS: Pack = Pack {
    id: "tutorials",
    summary: "step-by-step guides for every app",
    approx_mb: 20,
    license: "GPL-3.0-or-later",
    source: "docs/ in the source tree",
    priority: 0,
    app: "",
    required: true,
    replaces: &[],
    fetch: Fetch::Docs,
};

fn app_pack(app: &'static App) -> Pack {
    Pack {
        id: app.id,
        summary: app.summary,
        approx_mb: u64::from(app.approx_mb),
        license: "GPL-3.0-or-later",
        source: "your build of this repo (--bin-dir)",
        priority: 0,
        app: app.id,
        required: true,
        replaces: &[],
        fetch: Fetch::AppBinary,
    }
}

/// Every pack: one required pack per app, the tutorials, then the optional packs.
pub fn all() -> Vec<Pack> {
    let mut out: Vec<Pack> = APPS.iter().map(app_pack).collect();
    out.push(TUTORIALS);
    out.extend_from_slice(OPTIONAL);
    out
}

pub fn find<'a>(packs: &'a [Pack], id: &str) -> Option<&'a Pack> {
    packs.iter().find(|p| p.id == id)
}

/// How many MB an edition of `gb` may hold.
pub fn budget_mb(gb: u64) -> u64 {
    (gb as f64 * 1000.0 * USABLE) as u64
}

/// The biggest set of packs that fits `budget` MB: every required pack, then
/// optional packs by priority, skipping any that don't fit. A pack that
/// replaces smaller tiers only costs the difference. Returns None when the
/// required packs alone don't fit.
pub fn plan(packs: &[Pack], budget: u64) -> Option<Vec<Pack>> {
    let mut chosen: Vec<Pack> = packs.iter().filter(|p| p.required).copied().collect();
    let mut total: u64 = chosen.iter().map(|p| p.approx_mb).sum();
    if total > budget {
        return None;
    }
    let mut rest: Vec<Pack> = packs.iter().filter(|p| !p.required).copied().collect();
    rest.sort_by_key(|p| p.priority);
    for p in rest {
        if chosen.iter().any(|c| c.replaces.contains(&p.id)) {
            continue; // a bigger tier is already in
        }
        let freed: u64 = chosen.iter().filter(|c| p.replaces.contains(&c.id)).map(|c| c.approx_mb).sum();
        let after = total - freed + p.approx_mb;
        if after <= budget {
            chosen.retain(|c| !p.replaces.contains(&c.id));
            chosen.push(p);
            total = after;
        }
    }
    Some(chosen)
}

/// Total MB of a set once smaller tiers replaced by a bigger one are dropped.
pub fn needed_mb(packs: &[Pack]) -> u64 {
    packs
        .iter()
        .filter(|p| !packs.iter().any(|q| q.replaces.contains(&p.id)))
        .map(|p| p.approx_mb)
        .sum()
}

/// The smallest power-of-two edition (in GB) that holds all of `packs`:
/// start at 32 GB, double while it doesn't fit, halve while half still fits.
pub fn auto_size(packs: &[Pack]) -> u64 {
    let need = needed_mb(packs);
    let mut gb = START_GB;
    while budget_mb(gb) < need {
        gb *= 2;
    }
    while gb > 1 && budget_mb(gb / 2) >= need {
        gb /= 2;
    }
    gb
}

/// Edition sizes from 1 GB up to the one that holds everything.
pub fn edition_sizes(packs: &[Pack]) -> Vec<u64> {
    let top = auto_size(packs);
    let mut out = Vec::new();
    let mut gb = 1;
    while gb <= top {
        out.push(gb);
        gb *= 2;
    }
    out
}

pub fn is_power_of_two(gb: u64) -> bool {
    gb > 0 && gb & (gb - 1) == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fake(id: &'static str, mb: u64, priority: u32, required: bool, replaces: &'static [&'static str]) -> Pack {
        Pack {
            id,
            summary: "",
            approx_mb: mb,
            license: "CC0-1.0",
            source: "",
            priority,
            app: "",
            required,
            replaces,
            fetch: Fetch::Manual,
        }
    }

    fn ids(packs: &[Pack]) -> Vec<&'static str> {
        let mut v: Vec<_> = packs.iter().map(|p| p.id).collect();
        v.sort_unstable();
        v
    }

    fn sample() -> Vec<Pack> {
        vec![
            fake("app", 500, 0, true, &[]),
            fake("fonts", 1000, 1, false, &[]),
            fake("map-s", 2000, 2, false, &[]),
            fake("tex", 10_000, 3, false, &[]),
            fake("map-l", 40_000, 4, false, &["map-s"]),
            fake("tiny", 100, 5, false, &[]),
        ]
    }

    #[test]
    fn plan_fills_by_priority_and_skips_what_does_not_fit() {
        let p = sample();
        assert!(plan(&p, 400).is_none());
        assert_eq!(ids(&plan(&p, 550).unwrap()), ["app"]);
        assert_eq!(ids(&plan(&p, 600).unwrap()), ["app", "tiny"]);
        // map-s doesn't fit after app+fonts: skip it and keep going down the list
        assert_eq!(ids(&plan(&p, 2600).unwrap()), ["app", "fonts", "tiny"]);
        assert_eq!(ids(&plan(&p, 13_500).unwrap()), ["app", "fonts", "map-s", "tex"]);
    }

    #[test]
    fn bigger_tier_replaces_smaller_and_costs_the_difference() {
        let p = sample();
        // 500 + 1000 + 10000 + 40000 = 51500: map-l swaps out map-s
        let set = plan(&p, 51_500).unwrap();
        assert_eq!(ids(&set), ["app", "fonts", "map-l", "tex"]);
        assert_eq!(needed_mb(&set), 51_500);
        // a set naming both tiers only counts the big one
        assert_eq!(needed_mb(&p), 51_600);
    }

    #[test]
    fn auto_starts_at_32_and_doubles_or_halves() {
        assert_eq!(auto_size(&[fake("a", 31_000, 0, true, &[])]), 64); // 32 GB holds 30,400
        assert_eq!(auto_size(&[fake("a", 30_000, 0, true, &[])]), 32);
        assert_eq!(auto_size(&[fake("a", 100_000, 0, true, &[])]), 128);
        assert_eq!(auto_size(&[fake("a", 3_000, 0, true, &[])]), 4);
        assert_eq!(auto_size(&[fake("a", 10, 0, true, &[])]), 1);
        assert_eq!(auto_size(&sample()), 64);
        assert_eq!(edition_sizes(&sample()), [1, 2, 4, 8, 16, 32, 64]);
    }

    #[test]
    fn editions_only_grow() {
        let packs = all();
        let mut last = 0;
        for gb in edition_sizes(&packs) {
            let set = plan(&packs, budget_mb(gb)).expect("required packs fit in 1 GB");
            let mb = needed_mb(&set);
            assert!(mb <= budget_mb(gb));
            assert!(mb >= last);
            last = mb;
        }
        let biggest = plan(&packs, budget_mb(*edition_sizes(&packs).last().unwrap())).unwrap();
        assert!(find(&biggest, "maps-planet").is_some());
        assert!(find(&biggest, "maps-world-low").is_none());
    }

    #[test]
    fn real_packs_are_sane() {
        let packs = all();
        for (i, a) in packs.iter().enumerate() {
            for b in &packs[i + 1..] {
                assert_ne!(a.id, b.id);
            }
            for r in a.replaces {
                assert!(find(&packs, r).is_some(), "{} replaces unknown {r}", a.id);
            }
            let free = ["CC0", "OFL", "ODbL", "GPL"];
            assert!(free.iter().any(|f| a.license.contains(f)), "{} has licence {}", a.id, a.license);
        }
        for app in APPS {
            assert!(find(&packs, app.id).is_some_and(|p| p.required));
        }
        assert!(is_power_of_two(32) && !is_power_of_two(24) && !is_power_of_two(0));
    }
}
