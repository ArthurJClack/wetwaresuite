//! `hub install <tool> --from PATH`: installs a tool from a folder, a single
//! program file, or an archive (.zip, .tar, .tar.gz, .tgz, .tar.xz) that is
//! already on this computer, e.g. copied from a USB stick. Nothing is downloaded.

use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};

const ARCHIVES: &[&str] = &[".zip", ".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2"];

fn is_archive(path: &Path) -> bool {
    let name = path.file_name().map(|n| n.to_string_lossy().to_lowercase()).unwrap_or_default();
    ARCHIVES.iter().any(|ext| name.ends_with(ext))
}

/// Looks for `file_name` in `dir` and its subfolders (a few levels deep).
pub fn find_file(dir: &Path, file_name: &str, depth: usize) -> Option<PathBuf> {
    let direct = dir.join(file_name);
    if direct.is_file() {
        return Some(direct);
    }
    if depth == 0 {
        return None;
    }
    let mut subdirs: Vec<PathBuf> =
        std::fs::read_dir(dir).ok()?.filter_map(Result::ok).map(|e| e.path()).filter(|p| p.is_dir()).collect();
    subdirs.sort();
    subdirs.iter().find_map(|d| find_file(d, file_name, depth - 1))
}

/// Unpacks an archive with the system's `tar` (built into Windows 10+, macOS and
/// Linux; it reads .zip too except GNU tar, where `unzip` is tried instead).
fn extract(archive: &Path, into: &Path) -> Result<(), String> {
    std::fs::create_dir_all(into).map_err(|e| e.to_string())?;
    let quiet = |c: &mut Command| -> bool {
        c.stdout(Stdio::null()).stderr(Stdio::null()).status().map(|s| s.success()).unwrap_or(false)
    };
    if quiet(Command::new("tar").arg("-xf").arg(archive).arg("-C").arg(into)) {
        return Ok(());
    }
    let zip = archive.to_string_lossy().to_lowercase().ends_with(".zip");
    if zip && quiet(Command::new("unzip").arg("-q").arg("-o").arg(archive).arg("-d").arg(into)) {
        return Ok(());
    }
    Err(format!("couldn't unpack {} (tried `tar`{}). Unpack it yourself and pass the folder instead", archive.display(), if zip { " and `unzip`" } else { "" }))
}

fn copy_program(from: &Path, to: &Path) -> std::io::Result<()> {
    let mut tmp = to.as_os_str().to_os_string();
    tmp.push(".hub-tmp");
    let tmp = PathBuf::from(tmp);
    std::fs::copy(from, &tmp)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o755))?;
    }
    std::fs::rename(&tmp, to).inspect_err(|_| {
        let _ = std::fs::remove_file(&tmp);
    })
}

pub fn run(tool: &str, binary: &str, args: &[String], default_bin_dir: &Path, exe_name: fn(&str) -> String) -> ExitCode {
    let mut from: Option<PathBuf> = None;
    let mut to: Option<PathBuf> = None;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let slot = match a.as_str() {
            "--from" => &mut from,
            "--to" => &mut to,
            other => {
                eprintln!("unknown option '{other}'\nusage: hub install <tool> --from FOLDER|ARCHIVE|PROGRAM [--to FOLDER]");
                return ExitCode::from(2);
            }
        };
        match it.next() {
            Some(v) => *slot = Some(PathBuf::from(v)),
            None => {
                eprintln!("{a} needs a path");
                return ExitCode::from(2);
            }
        }
    }
    let Some(from) = from else {
        eprintln!("usage: hub install <tool> --from FOLDER|ARCHIVE|PROGRAM [--to FOLDER]");
        return ExitCode::from(2);
    };
    let dest_dir = to.unwrap_or_else(|| default_bin_dir.to_path_buf());
    let program = exe_name(binary);

    let mut scratch: Option<PathBuf> = None;
    let source = if from.is_dir() {
        find_file(&from, &program, 4)
    } else if from.is_file() && is_archive(&from) {
        let dir = std::env::temp_dir().join(format!("hub-install-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        if let Err(e) = extract(&from, &dir) {
            eprintln!("{e}.");
            return ExitCode::FAILURE;
        }
        scratch = Some(dir.clone());
        find_file(&dir, &program, 4)
    } else if from.is_file() {
        Some(from.clone())
    } else {
        eprintln!("{} doesn't exist.", from.display());
        return ExitCode::FAILURE;
    };
    let cleanup = || {
        if let Some(d) = &scratch {
            let _ = std::fs::remove_dir_all(d);
        }
    };
    let Some(source) = source else {
        eprintln!("Couldn't find {program} (the program for {tool}) in {}.", from.display());
        cleanup();
        return ExitCode::FAILURE;
    };
    if let Err(e) = std::fs::create_dir_all(&dest_dir) {
        eprintln!("Couldn't make {}: {e}.", dest_dir.display());
        cleanup();
        return ExitCode::FAILURE;
    }
    let dest = dest_dir.join(&program);
    let same = matches!((std::fs::canonicalize(&source), std::fs::canonicalize(&dest)), (Ok(a), Ok(b)) if a == b);
    let result = if same { Ok(()) } else { copy_program(&source, &dest) };
    cleanup();
    match result {
        Ok(()) if same => println!("{tool} is already installed at {} (nothing to copy).", dest.display()),
        Ok(()) => {
            println!("Installed {tool}: copied {program} to {}. Nothing was downloaded.", dest_dir.display());
            println!("  Try it: {} --version   Connect it to Claude (optional): hub connect", dest.display());
        }
        Err(e) => {
            eprintln!("Couldn't copy {} to {}: {e}.", source.display(), dest.display());
            return ExitCode::FAILURE;
        }
    }
    ExitCode::SUCCESS
}

#[cfg(test)]
mod tests {
    use super::*;

    fn exe(b: &str) -> String {
        if cfg!(windows) {
            format!("{b}.exe")
        } else {
            b.to_string()
        }
    }

    #[test]
    fn installs_from_a_folder() {
        let root = std::env::temp_dir().join(format!("hub-local-install-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let src = root.join("usb").join("wetwaresuite-0.6").join("bin");
        std::fs::create_dir_all(&src).unwrap();
        std::fs::write(src.join(exe("wetwaresuite")), "program").unwrap();
        let dest = root.join("installed");
        let args = vec!["--from".to_string(), root.join("usb").to_string_lossy().into(), "--to".into(), dest.to_string_lossy().into()];
        assert_eq!(run("photo", "wetwaresuite", &args, &dest, exe), ExitCode::SUCCESS);
        assert_eq!(std::fs::read_to_string(dest.join(exe("wetwaresuite"))).unwrap(), "program");
        // again: same result, still fine
        assert_eq!(run("photo", "wetwaresuite", &args, &dest, exe), ExitCode::SUCCESS);
        // a folder without the program fails clearly
        let empty = vec!["--from".to_string(), dest.join("nope").to_string_lossy().into()];
        assert_eq!(run("photo", "wetwaresuite", &empty, &dest, exe), ExitCode::FAILURE);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn spots_archives() {
        assert!(is_archive(Path::new("a/wetwaresuite-linux.tar.gz")));
        assert!(is_archive(Path::new("W.ZIP")));
        assert!(!is_archive(Path::new("wetwaresuite")));
    }
}
