//! Offline installs: copy app binaries (and content packs) from a local folder
//! or archive. Nothing here touches the network.
//!
//! A source is any folder that has the binaries somewhere inside (a release
//! folder, a build folder, or a bundle made by `hub bundle`), or an archive of
//! one: `.tar` is read here, `.zip`/`.tar.gz`/`.tgz`/`.tar.xz` are unpacked with
//! the system `tar` (built into Windows 10+, macOS and Linux).

use std::io::Read;
use std::path::{Component, Path, PathBuf};

use crate::catalog::{self, App, APPS};
use crate::paths;

/// An opened source; an unpacked archive is deleted again when this drops.
pub struct Source {
    pub root: PathBuf,
    temp: Option<PathBuf>,
}

impl Drop for Source {
    fn drop(&mut self) {
        if let Some(t) = &self.temp {
            let _ = std::fs::remove_dir_all(t);
        }
    }
}

impl Source {
    pub fn is_bundle(&self) -> bool {
        self.root.join("bundle.txt").is_file()
    }

    /// Finds `binary` (without .exe) in the source: top level, bin/, or a few folders down.
    pub fn find_binary(&self, binary: &str) -> Option<PathBuf> {
        find_file(&self.root, &paths::exe_name(binary), 5)
    }

    /// Pack folders in a bundle (`packs/<id>/`).
    pub fn packs(&self) -> Vec<String> {
        let mut out: Vec<String> = std::fs::read_dir(self.root.join("packs"))
            .map(|rd| {
                rd.flatten()
                    .filter(|e| e.path().is_dir())
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .collect()
            })
            .unwrap_or_default();
        out.sort();
        out
    }
}

fn find_file(dir: &Path, name: &str, depth: u32) -> Option<PathBuf> {
    let direct = dir.join(name);
    if direct.is_file() {
        return Some(direct);
    }
    let bin = dir.join("bin").join(name);
    if bin.is_file() {
        return Some(bin);
    }
    if depth == 0 {
        return None;
    }
    let mut subdirs: Vec<PathBuf> = std::fs::read_dir(dir)
        .ok()?
        .flatten()
        .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()))
        .map(|e| e.path())
        .filter(|p| {
            let n = p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            // skip build noise and content packs: they never hold binaries
            !n.starts_with('.') && n != "CMakeFiles" && n != "packs" && n != "_deps" && n != "target"
        })
        .collect();
    subdirs.sort();
    subdirs.into_iter().find_map(|d| find_file(&d, name, depth - 1))
}

/// Where to look when no `--from` is given: the hub's own folder (a release or
/// bundle folder ships hub next to the apps) and its parent.
pub fn default_source() -> Result<Source, String> {
    let here = paths::self_dir();
    let mut candidates = vec![here.clone()];
    if let Some(parent) = here.parent() {
        candidates.push(parent.to_path_buf());
    }
    for c in candidates {
        let src = Source { root: c, temp: None };
        if src.is_bundle() || APPS.iter().any(|a| src.find_binary(&a.binary()).is_some()) {
            return Ok(src);
        }
    }
    Err(format!(
        "no app packages next to hub ({}). Pass --from with a folder or archive that has them: \
         a release download, an offline edition made with `hub bundle`, or your build folder. \
         hub never downloads during install.",
        here.display()
    ))
}

pub fn open_source(from: &Path) -> Result<Source, String> {
    if from.is_dir() {
        return Ok(Source { root: from.to_path_buf(), temp: None });
    }
    if !from.is_file() {
        return Err(format!("{} doesn't exist", from.display()));
    }
    let temp = std::env::temp_dir().join(format!("wetwaresuite-unpack-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&temp);
    std::fs::create_dir_all(&temp).map_err(|e| format!("can't make {}: {e}", temp.display()))?;
    let src = Source { root: temp.clone(), temp: Some(temp.clone()) };
    let name = from.to_string_lossy().to_ascii_lowercase();
    if name.ends_with(".tar") {
        let mut f = std::fs::File::open(from).map_err(|e| format!("can't read {}: {e}", from.display()))?;
        untar(&mut f, &temp).map_err(|e| format!("can't unpack {}: {e}", from.display()))?;
    } else {
        let status = std::process::Command::new("tar")
            .arg("-xf")
            .arg(from)
            .arg("-C")
            .arg(&temp)
            .status()
            .map_err(|e| format!("can't run tar to unpack {}: {e}. Unpack it yourself and pass the folder", from.display()))?;
        if !status.success() {
            return Err(format!("tar couldn't unpack {}. Unpack it yourself and pass the folder", from.display()));
        }
    }
    // an archive of one folder (release/, wetwaresuite-32gb/...): use that folder
    let entries: Vec<PathBuf> = std::fs::read_dir(&temp)
        .map(|rd| rd.flatten().map(|e| e.path()).collect())
        .unwrap_or_default();
    let mut src = src;
    if entries.len() == 1 && entries[0].is_dir() {
        src.root = entries[0].clone();
    }
    Ok(src)
}

fn safe_relative(name: &str) -> Option<PathBuf> {
    let p = Path::new(name);
    let mut out = PathBuf::new();
    for c in p.components() {
        match c {
            Component::Normal(part) => out.push(part),
            Component::CurDir => {}
            _ => return None, // absolute paths and .. never leave the target folder
        }
    }
    (!out.as_os_str().is_empty()).then_some(out)
}

fn octal(field: &[u8]) -> u64 {
    field
        .iter()
        .skip_while(|b| **b == b' ')
        .take_while(|b| (b'0'..=b'7').contains(b))
        .fold(0, |n, b| n * 8 + u64::from(b - b'0'))
}

fn cstr(field: &[u8]) -> String {
    let end = field.iter().position(|b| *b == 0).unwrap_or(field.len());
    String::from_utf8_lossy(&field[..end]).into_owned()
}

/// Unpacks a plain (ustar/GNU) tar stream into `to`. Returns the number of files.
pub fn untar(input: &mut dyn Read, to: &Path) -> std::io::Result<usize> {
    let bad = |m: &str| std::io::Error::new(std::io::ErrorKind::InvalidData, m.to_string());
    let mut files = 0;
    let mut long_name: Option<String> = None;
    let mut header = [0u8; 512];
    loop {
        if input.read_exact(&mut header).is_err() || header.iter().all(|b| *b == 0) {
            break;
        }
        let size = octal(&header[124..136]);
        let kind = header[156];
        let mut name = cstr(&header[0..100]);
        if &header[257..262] == b"ustar" {
            let prefix = cstr(&header[345..500]);
            if !prefix.is_empty() {
                name = format!("{prefix}/{name}");
            }
        }
        if let Some(l) = long_name.take() {
            name = l;
        }
        let padded = size.div_ceil(512) * 512;
        let mut data = vec![0u8; usize::try_from(padded).map_err(|_| bad("entry too big"))?];
        input.read_exact(&mut data)?;
        data.truncate(size as usize);
        match kind {
            b'L' => long_name = Some(cstr(&data)),
            b'0' | 0 | b'7' => {
                let rel = safe_relative(&name).ok_or_else(|| bad("unsafe path in archive"))?;
                let target = to.join(rel);
                if let Some(parent) = target.parent() {
                    std::fs::create_dir_all(parent)?;
                }
                std::fs::write(&target, &data)?;
                #[cfg(unix)]
                {
                    use std::os::unix::fs::PermissionsExt;
                    let mode = octal(&header[100..108]) as u32 & 0o777;
                    std::fs::set_permissions(&target, std::fs::Permissions::from_mode(mode.max(0o644)))?;
                }
                files += 1;
            }
            b'5' => {
                if let Some(rel) = safe_relative(&name) {
                    std::fs::create_dir_all(to.join(rel))?;
                }
            }
            _ => {} // links, pax headers: not needed for our packages
        }
    }
    Ok(files)
}

fn make_executable(_path: &Path) {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(_path, std::fs::Permissions::from_mode(0o755));
    }
}

/// Copies one binary from the source into `dest`. Returns where it went.
pub fn install_binary(src: &Source, binary: &str, dest: &Path) -> Result<PathBuf, String> {
    let found = src
        .find_binary(binary)
        .ok_or_else(|| format!("{} isn't in {}", paths::exe_name(binary), src.root.display()))?;
    std::fs::create_dir_all(dest).map_err(|e| format!("can't make {}: {e}", dest.display()))?;
    let target = dest.join(paths::exe_name(binary));
    if found != target {
        std::fs::copy(&found, &target).map_err(|e| format!("can't copy to {}: {e}", target.display()))?;
    }
    make_executable(&target);
    Ok(target)
}

/// Copies a bundle's pack folder into the per-user packs folder.
pub fn install_pack(src: &Source, id: &str, dest: &Path) -> Result<u64, String> {
    let from = src.root.join("packs").join(id);
    if !from.is_dir() {
        return Err(format!("pack {id} isn't in {}", src.root.display()));
    }
    paths::copy_tree(&from, &dest.join(id)).map_err(|e| format!("can't copy pack {id}: {e}"))
}

/// What the names on the command line mean: apps, tools (as aliases), `all`,
/// or `suite` for the all-in-one binary. `files` always comes along.
pub fn resolve(names: &[String]) -> Result<(Vec<&'static App>, bool), String> {
    let mut apps: Vec<&'static App> = Vec::new();
    let mut suite = false;
    for n in names {
        let lower = n.to_ascii_lowercase();
        if lower == "all" {
            apps = APPS.iter().collect();
            continue;
        }
        if lower == "suite" || lower == catalog::SUITE {
            suite = true;
            continue;
        }
        let app = catalog::find_app(n).ok_or_else(|| format!("no app or tool called '{n}', run `hub list` to see them"))?;
        if !apps.iter().any(|a| a.id == app.id) {
            apps.push(app);
        }
    }
    if !apps.is_empty() || !suite {
        for a in APPS.iter().filter(|a| a.always) {
            if !apps.iter().any(|x| x.id == a.id) {
                apps.push(a);
            }
        }
    }
    Ok((apps, suite))
}

/// Where an app is installed, if anywhere: the install folder, next to hub, or PATH.
pub fn installed_at(binary: &str, dest: &Path) -> Option<PathBuf> {
    let name = paths::exe_name(binary);
    let mut dirs = vec![dest.to_path_buf(), paths::self_dir()];
    if let Some(path) = std::env::var_os("PATH") {
        dirs.extend(std::env::split_paths(&path));
    }
    dirs.into_iter().map(|d| d.join(&name)).find(|p| p.is_file())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tar_entry(out: &mut Vec<u8>, name: &str, data: &[u8], kind: u8) {
        let mut h = [0u8; 512];
        h[..name.len()].copy_from_slice(name.as_bytes());
        h[100..107].copy_from_slice(b"0000755");
        let size = format!("{:011o}", data.len());
        h[124..135].copy_from_slice(size.as_bytes());
        h[156] = kind;
        h[257..262].copy_from_slice(b"ustar");
        out.extend_from_slice(&h);
        out.extend_from_slice(data);
        out.resize(out.len().div_ceil(512) * 512, 0);
    }

    fn temp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("hub-test-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn untar_reads_files_dirs_and_long_names() {
        let mut tar = Vec::new();
        tar_entry(&mut tar, "release/bin/", b"", b'5');
        tar_entry(&mut tar, "release/bin/wetwaresuite-image", b"#!fake", b'0');
        let long = format!("release/{}/x.txt", "d".repeat(120));
        tar_entry(&mut tar, "././@LongLink", format!("{long}\0").as_bytes(), b'L');
        tar_entry(&mut tar, "ignored", b"hello", b'0');
        tar.extend_from_slice(&[0u8; 1024]);
        let dir = temp("untar");
        assert_eq!(untar(&mut tar.as_slice(), &dir).unwrap(), 2);
        assert_eq!(std::fs::read(dir.join("release/bin/wetwaresuite-image")).unwrap(), b"#!fake");
        assert_eq!(std::fs::read(dir.join(&long)).unwrap(), b"hello");

        let mut evil = Vec::new();
        tar_entry(&mut evil, "../escape", b"x", b'0');
        assert!(untar(&mut evil.as_slice(), &dir).is_err());
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn install_from_folder_and_tar_archive_offline() {
        let src_dir = temp("src");
        std::fs::create_dir_all(src_dir.join("build/apps/image")).unwrap();
        let bin = paths::exe_name("wetwaresuite-image");
        std::fs::write(src_dir.join("build/apps/image").join(&bin), b"img").unwrap();
        std::fs::create_dir_all(src_dir.join("packs/fonts")).unwrap();
        std::fs::write(src_dir.join("packs/fonts/a.ttf"), b"font").unwrap();
        std::fs::write(src_dir.join("bundle.txt"), b"edition=1\n").unwrap();

        let dest = temp("dest");
        let src = open_source(&src_dir).unwrap();
        assert!(src.is_bundle());
        assert_eq!(src.packs(), ["fonts"]);
        let to = install_binary(&src, "wetwaresuite-image", &dest).unwrap();
        assert_eq!(std::fs::read(&to).unwrap(), b"img");
        assert!(install_binary(&src, "wetwaresuite-model", &dest).is_err());
        assert_eq!(install_pack(&src, "fonts", &dest.join("packs")).unwrap(), 4);
        assert!(installed_at("wetwaresuite-image", &dest).is_some());

        let mut tar = Vec::new();
        tar_entry(&mut tar, &format!("rel/{bin}"), b"img2", b'0');
        tar.extend_from_slice(&[0u8; 1024]);
        let archive = src_dir.join("release.tar");
        std::fs::write(&archive, &tar).unwrap();
        let unpacked_root;
        {
            let src = open_source(&archive).unwrap();
            assert!(src.root.ends_with("rel"), "a one-folder archive opens at that folder");
            unpacked_root = src.root.parent().unwrap().to_path_buf();
            install_binary(&src, "wetwaresuite-image", &dest).unwrap();
        }
        assert!(!unpacked_root.exists(), "unpacked archive is cleaned up");
        assert_eq!(std::fs::read(dest.join(&bin)).unwrap(), b"img2");
        let _ = std::fs::remove_dir_all(src_dir);
        let _ = std::fs::remove_dir_all(dest);
    }

    #[test]
    fn names_resolve_to_apps_with_files_always() {
        let (apps, suite) = resolve(&["paint".into(), "image".into(), "slicer".into()]).unwrap();
        let ids: Vec<_> = apps.iter().map(|a| a.id).collect();
        assert_eq!(ids, ["image", "cad", "files"]);
        assert!(!suite);
        let (apps, suite) = resolve(&["suite".into()]).unwrap();
        assert!(apps.is_empty() && suite);
        assert_eq!(resolve(&["all".into()]).unwrap().0.len(), APPS.len());
        assert!(resolve(&["photoshop".into()]).is_err());
    }
}
