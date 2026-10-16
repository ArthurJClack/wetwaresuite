//! `hub bundle`: makes an offline edition folder. This is a build step and may
//! download optional packs; installing from the result (`hub install --from`)
//! never does.
//!
//! Layout of an edition:
//!
//! ```text
//! bin/            hub and every wetwaresuite-<app> binary
//! docs/           the tutorials
//! packs/<id>/     content packs (fonts, samples, textures, map tiles...)
//! bundle.txt      what's inside: edition size and one line per pack
//! LICENSES.txt    each pack's licence and source
//! README.txt      how to install from it
//! ```

use std::path::{Path, PathBuf};
use std::process::Command;

use crate::catalog::APPS;
use crate::packs::{self, Fetch, Pack};
use crate::paths;

pub enum Size {
    Auto,
    Gb(u64),
}

pub struct Options {
    pub size: Size,
    pub out: PathBuf,
    /// Only these optional packs (required ones are always in). None = all.
    pub packs: Option<Vec<String>>,
    /// Folders to take app binaries from (searched in order). Empty = next to hub.
    pub bin_dirs: Vec<PathBuf>,
    pub docs: Option<PathBuf>,
    /// Folder of already-downloaded packs (`<cache>/<id>/`), used before any download.
    pub cache: Option<PathBuf>,
    pub dry_run: bool,
}

/// The packs a bundle chooses from, after `--packs`.
pub fn candidates(filter: Option<&[String]>) -> Result<Vec<Pack>, String> {
    let all = packs::all();
    let Some(filter) = filter else { return Ok(all) };
    for id in filter {
        if packs::find(&all, id).is_none() {
            return Err(format!("no pack called '{id}', run `hub editions` to see them"));
        }
    }
    Ok(all.into_iter().filter(|p| p.required || filter.iter().any(|f| f == p.id)).collect())
}

/// Chooses the edition size and its packs.
pub fn choose(size: &Size, candidates: &[Pack]) -> Result<(u64, Vec<Pack>), String> {
    let gb = match size {
        Size::Auto => packs::auto_size(candidates),
        Size::Gb(gb) => {
            if !packs::is_power_of_two(*gb) {
                return Err(format!("editions come in powers of two (1, 2, 4, 8, 16, 32, 64...), not {gb}"));
            }
            *gb
        }
    };
    let set = packs::plan(candidates, packs::budget_mb(gb)).ok_or_else(|| {
        let need: u64 = candidates.iter().filter(|p| p.required).map(|p| p.approx_mb).sum();
        format!("the apps and tutorials alone need about {need} MB, more than a {gb} GB edition holds")
    })?;
    Ok((gb, set))
}

pub fn describe(gb: u64, set: &[Pack]) -> String {
    let mut out = format!(
        "{gb} GB edition: about {:.1} GB of {:.1} GB usable\n",
        packs::needed_mb(set) as f64 / 1000.0,
        packs::budget_mb(gb) as f64 / 1000.0
    );
    for p in set {
        out.push_str(&format!("  {:<16} {:>8} MB  {}\n", p.id, p.approx_mb, p.summary));
    }
    out
}

fn run(cmd: &mut Command) -> Result<(), String> {
    let shown = format!("{cmd:?}");
    match cmd.status() {
        Ok(s) if s.success() => Ok(()),
        Ok(s) => Err(format!("{shown} failed ({s})")),
        Err(e) => Err(format!("can't run {shown}: {e}")),
    }
}

fn download(url: &str, to: &Path) -> Result<(), String> {
    run(Command::new("curl").args(["-fL", "--retry", "3", "-o"]).arg(to).arg(url))
}

/// Unpacks a downloaded archive in place (system tar) and removes it.
fn unpack_in_place(file: &Path) -> Result<(), String> {
    let name = file.to_string_lossy().to_ascii_lowercase();
    if [".zip", ".tar", ".tar.gz", ".tgz", ".tar.xz"].iter().any(|e| name.ends_with(e)) {
        let dir = file.parent().unwrap_or(Path::new("."));
        run(Command::new("tar").arg("-xf").arg(file).arg("-C").arg(dir))?;
        let _ = std::fs::remove_file(file);
    }
    Ok(())
}

fn file_name_of(url: &str) -> String {
    let tail = url.split(['?', '#']).next().unwrap_or(url).rsplit('/').next().unwrap_or("");
    if tail.is_empty() {
        "download".into()
    } else {
        tail.into()
    }
}

/// The links on lines that contain `filter`, in a CSV or text list.
pub fn links_in(list: &str, filter: &str) -> Vec<String> {
    let mut out = Vec::new();
    for line in list.lines().filter(|l| l.contains(filter)) {
        for token in line.split([',', ';', ' ', '\t', '"']) {
            if token.starts_with("https://") && !out.iter().any(|o| o == token) {
                out.push(token.to_string());
            }
        }
    }
    out
}

/// Starter files each app makes for itself (CC0): batch scripts with `@OUT@`
/// standing for the pack folder.
pub const SAMPLES: &[(&str, &str)] = &[
    (
        "image",
        "paint.new width=512 height=512 background=\"#6b4f2a\" -> t\n\
         paint.stroke doc=$t points=0,128,512,140 color=\"#4a3418\" size=18\n\
         paint.stroke doc=$t points=0,300,512,290 color=\"#4a3418\" size=14\n\
         photo.save doc=$t path=\"@OUT@/image/wood-texture.png\"\n",
    ),
    (
        "draw",
        "vector.new width=256 height=256 -> v\n\
         vector.add_rect doc=$v x=28 y=28 width=200 height=200 radius=24 fill=\"#2a6fdb\"\n\
         vector.save doc=$v path=\"@OUT@/draw/logo.svg\"\n",
    ),
    ("model", "sculpt.new shape=sphere -> s\nsculpt.export doc=$s path=\"@OUT@/model/sphere.obj\"\n"),
    ("cad", "cad.new -> c\ncad.box doc=$c width=40 depth=20 height=10\ncad.save doc=$c path=\"@OUT@/cad/block.stl\"\n"),
    ("sound", "beats.generate genre=house -> b\nbeats.render song=$b path=\"@OUT@/sound/house-loop.wav\"\n"),
    (
        "game",
        "levels.new width=48 height=32 -> l\n\
         levels.tileset_placeholder doc=$l path=\"@OUT@/game/tiles.png\" tiles=4\n\
         levels.gen_caves doc=$l wall=1 floor=2 seed=7\n\
         levels.save doc=$l path=\"@OUT@/game/cave.tmj\"\n",
    ),
    ("maps", "maps.new title=World -> m\nmaps.add_countries doc=$m\nmaps.save doc=$m path=\"@OUT@/maps/world.wwmap\"\n"),
];

fn generate_samples(out: &Path, dir: &Path) -> Result<(), String> {
    let mut made = 0;
    for (app, script) in SAMPLES {
        let bin = out.join("bin").join(paths::exe_name(&format!("wetwaresuite-{app}")));
        if !bin.is_file() {
            continue;
        }
        std::fs::create_dir_all(dir.join(app)).map_err(|e| e.to_string())?;
        let text = script.replace("@OUT@", &dir.to_string_lossy().replace('\\', "/"));
        let script_file = dir.join(format!("{app}.wws"));
        std::fs::write(&script_file, text).map_err(|e| e.to_string())?;
        let quiet = Command::new(&bin)
            .arg("batch")
            .arg(&script_file)
            .arg("--keep-going")
            .stdout(std::process::Stdio::null())
            .status();
        match quiet.map_err(|e| e.to_string()).and_then(|s| if s.success() { Ok(()) } else { Err(s.to_string()) }) {
            Ok(()) => made += 1,
            Err(e) => eprintln!("  samples for {app}: {e}"),
        }
    }
    if made == 0 {
        return Err("no app binaries in the bundle to make samples with".into());
    }
    Ok(())
}

fn fetch(p: &Pack, opts: &Options, dir: &Path) -> Result<(), String> {
    if let Some(cache) = &opts.cache {
        let cached = cache.join(p.id);
        if cached.is_dir() {
            println!("  {}: copying from {}", p.id, cached.display());
            return paths::copy_tree(&cached, dir).map(|_| ()).map_err(|e| e.to_string());
        }
    }
    std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    match p.fetch {
        Fetch::AppBinary | Fetch::Docs => Ok(()), // handled by the caller
        Fetch::Generate => generate_samples(&opts.out, dir),
        Fetch::Url => {
            let file = dir.join(file_name_of(p.source));
            download(p.source, &file)?;
            unpack_in_place(&file)
        }
        Fetch::List { filter } => {
            let list = dir.join("list.csv");
            download(p.source, &list)?;
            let text = std::fs::read_to_string(&list).map_err(|e| e.to_string())?;
            let _ = std::fs::remove_file(&list);
            let links = links_in(&text, filter);
            if links.is_empty() {
                return Err(format!("no {filter} links in {}", p.source));
            }
            for url in links {
                let file = dir.join(file_name_of(&url));
                download(&url, &file)?;
                unpack_in_place(&file)?;
            }
            Ok(())
        }
        Fetch::Pmtiles { maxzoom } => run(Command::new("pmtiles")
            .arg("extract")
            .arg(p.source)
            .arg(dir.join("world.pmtiles"))
            .arg(format!("--maxzoom={maxzoom}")))
        .map_err(|e| format!("{e} (get the pmtiles tool from https://github.com/protomaps/go-pmtiles/releases)")),
        Fetch::Manual => Err(format!("download it from {} into <cache>/{}/ and pass --cache", p.source, p.id)),
    }
}

/// Where app binaries come from: every `--bin-dir`, else next to hub.
fn binary_sources(opts: &Options) -> Result<Vec<crate::install::Source>, String> {
    let dirs = if opts.bin_dirs.is_empty() { vec![paths::self_dir()] } else { opts.bin_dirs.clone() };
    dirs.iter().map(|d| crate::install::open_source(d)).collect()
}

fn copy_binary(sources: &[crate::install::Source], binary: &str, bin: &Path) -> Result<(), String> {
    let mut last = String::new();
    for src in sources {
        match crate::install::install_binary(src, binary, bin) {
            Ok(_) => return Ok(()),
            Err(e) => last = e,
        }
    }
    Err(last)
}

pub fn bundle(opts: &Options) -> Result<(), String> {
    let cands = candidates(opts.packs.as_deref())?;
    let (gb, set) = choose(&opts.size, &cands)?;
    print!("{}", describe(gb, &set));
    if opts.dry_run {
        return Ok(());
    }
    let out = &opts.out;
    let bin = out.join("bin");
    std::fs::create_dir_all(&bin).map_err(|e| format!("can't make {}: {e}", bin.display()))?;
    let sources = binary_sources(opts)?;
    let mut failed_required = Vec::new();
    let mut done: Vec<&Pack> = Vec::new();

    // apps first: the samples pack runs them
    for p in set.iter().filter(|p| p.fetch == Fetch::AppBinary) {
        let app = APPS.iter().find(|a| a.id == p.id).expect("app packs come from APPS");
        match copy_binary(&sources, &app.binary(), &bin) {
            Ok(()) => done.push(p),
            Err(e) => {
                eprintln!("  {}: {e}", p.id);
                failed_required.push(p.id);
            }
        }
    }
    for extra in ["hub", crate::catalog::SUITE] {
        let _ = copy_binary(&sources, extra, &bin);
    }
    if !bin.join(paths::exe_name("hub")).is_file() {
        if let Ok(me) = std::env::current_exe() {
            let _ = std::fs::copy(me, bin.join(paths::exe_name("hub")));
        }
    }
    for p in set.iter().filter(|p| p.fetch != Fetch::AppBinary) {
        let result = if p.fetch == Fetch::Docs {
            let docs = opts.docs.clone().unwrap_or_else(|| PathBuf::from("docs"));
            paths::copy_tree(&docs, &out.join("docs"))
                .map(|_| ())
                .map_err(|e| format!("can't copy {}: {e} (pass --docs <repo>/docs)", docs.display()))
        } else {
            if p.fetch == Fetch::Generate {
                println!("  {}: making them with the apps (no download)", p.id);
            } else {
                println!("  {}: fetching from {}", p.id, p.source);
            }
            fetch(p, opts, &out.join("packs").join(p.id))
        };
        match result {
            Ok(()) => done.push(p),
            Err(e) => {
                eprintln!("  {}: {e}", p.id);
                let _ = std::fs::remove_dir_all(out.join("packs").join(p.id));
                if p.required {
                    failed_required.push(p.id);
                }
            }
        }
    }

    let mut manifest = format!("edition={gb}\n");
    let mut licenses = String::from("Licences of the content in this edition\n\n");
    for p in &done {
        manifest.push_str(&format!("pack={}\t{}\t{}\n", p.id, p.approx_mb, p.app));
        licenses.push_str(&format!("{}: {}\n  source: {}\n\n", p.id, p.license, p.source));
    }
    let readme = format!(
        "wetwaresuite offline edition ({gb} GB)\n\n\
         Install everything (no internet needed):\n  bin/hub install --from .\n\n\
         Or only some apps, e.g.:\n  bin/hub install image model --from .\n\n\
         The apps also run straight from bin/ without installing.\n"
    );
    for (name, text) in [("bundle.txt", manifest), ("LICENSES.txt", licenses), ("README.txt", readme)] {
        std::fs::write(out.join(name), text).map_err(|e| format!("can't write {name}: {e}"))?;
    }
    println!("wrote {} ({} of {} packs)", out.display(), done.len(), set.len());
    if failed_required.is_empty() {
        Ok(())
    } else {
        Err(format!(
            "required packs missing: {} (build them and pass each folder with --bin-dir, e.g. --bin-dir build/bin \
             --bin-dir share/target/release)",
            failed_required.join(", ")
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn auto_holds_everything_and_sizes_must_be_powers_of_two() {
        let all = candidates(None).unwrap();
        let (gb, set) = choose(&Size::Auto, &all).unwrap();
        assert!(packs::is_power_of_two(gb));
        assert!(packs::find(&set, "maps-planet").is_some());
        assert!(choose(&Size::Gb(24), &all).is_err());
        let (gb, set) = choose(&Size::Gb(1), &all).unwrap();
        assert_eq!(gb, 1);
        assert!(packs::find(&set, "tutorials").is_some());
        assert!(packs::find(&set, "textures-1k").is_none());
    }

    #[test]
    fn packs_filter_keeps_required_and_shrinks_auto() {
        let only = candidates(Some(&["fonts".to_string()])).unwrap();
        assert!(only.iter().all(|p| p.required || p.id == "fonts"));
        let (gb, set) = choose(&Size::Auto, &only).unwrap();
        assert!(gb <= 2, "apps + fonts fit a small edition, got {gb}");
        assert!(packs::find(&set, "fonts").is_some());
        assert!(candidates(Some(&["nope".to_string()])).is_err());
    }

    #[test]
    fn list_links_are_filtered() {
        let csv = "assetId,downloadAttribute,link\nWood001,1K-JPG,https://x.test/Wood001_1K-JPG.zip\n\
                   Wood001,2K-JPG,https://x.test/Wood001_2K-JPG.zip\n";
        assert_eq!(links_in(csv, "1K-JPG"), ["https://x.test/Wood001_1K-JPG.zip"]);
        assert_eq!(file_name_of("https://x.test/a/b.zip?x=1"), "b.zip");
    }

    #[test]
    fn dry_run_touches_nothing() {
        let out = std::env::temp_dir().join(format!("hub-dry-{}", std::process::id()));
        let opts = Options {
            size: Size::Gb(8),
            out: out.clone(),
            packs: None,
            bin_dirs: Vec::new(),
            docs: None,
            cache: None,
            dry_run: true,
        };
        bundle(&opts).unwrap();
        assert!(!out.exists());
    }
}
