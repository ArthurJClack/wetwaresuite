//! Per-user folders, the same ones the C++ apps use (apps/common/handoff.cpp):
//! `$WETWARESUITE_HOME`, else ~/.local/share/wetwaresuite (or $XDG_DATA_HOME),
//! ~/Library/Application Support/wetwaresuite on macOS, %APPDATA%\wetwaresuite on Windows.

use std::path::{Path, PathBuf};

pub fn data_root() -> PathBuf {
    if let Some(home) = std::env::var_os("WETWARESUITE_HOME").filter(|v| !v.is_empty()) {
        return PathBuf::from(home);
    }
    if cfg!(windows) {
        if let Some(appdata) = std::env::var_os("APPDATA") {
            return PathBuf::from(appdata).join("wetwaresuite");
        }
    }
    let home = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
    if cfg!(target_os = "macos") {
        return home.join("Library/Application Support/wetwaresuite");
    }
    if let Some(xdg) = std::env::var_os("XDG_DATA_HOME").filter(|v| !v.is_empty()) {
        return PathBuf::from(xdg).join("wetwaresuite");
    }
    home.join(".local/share/wetwaresuite")
}

/// Where `hub install` puts app binaries by default.
pub fn bin_dir() -> PathBuf {
    data_root().join("bin")
}

pub fn packs_dir() -> PathBuf {
    data_root().join("packs")
}

pub fn exe_name(binary: &str) -> String {
    if cfg!(windows) {
        format!("{binary}.exe")
    } else {
        binary.to_string()
    }
}

/// The folder the running hub is in.
pub fn self_dir() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// Copies a folder tree. Existing files are overwritten.
pub fn copy_tree(from: &Path, to: &Path) -> std::io::Result<u64> {
    let mut bytes = 0;
    std::fs::create_dir_all(to)?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        let target = to.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            bytes += copy_tree(&entry.path(), &target)?;
        } else {
            bytes += std::fs::copy(entry.path(), &target)?;
        }
    }
    Ok(bytes)
}
