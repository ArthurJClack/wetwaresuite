//! Turning a path from the network into a file inside the shared folder, and
//! nothing else. Every request path goes through `clean_rel` first, then
//! `resolve_existing` (reads) or `resolve_for_write` (uploads, deletes).
//!
//! Rules, the same on every OS so a share behaves identically everywhere:
//! - relative only: no leading `/`, no drive letters (`C:`), no UNC (`\\host`)
//! - no `..` or `.` segments, no empty segments, no backslashes
//! - no characters Windows can't store (`<>:"|?*`, control chars), no
//!   trailing dots or spaces, no reserved device names (`CON`, `NUL`, `COM1`...)
//! - after resolving symlinks the result must still be inside the root

use std::fmt;
use std::io;
use std::path::{Path, PathBuf};

#[derive(Debug, PartialEq, Eq)]
pub enum PathError {
    Invalid(String),
    Outside,
    NotFound,
    Io(String),
}

impl fmt::Display for PathError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            PathError::Invalid(why) => write!(f, "invalid path: {why}"),
            PathError::Outside => write!(f, "path is outside the shared folder"),
            PathError::NotFound => write!(f, "not found"),
            PathError::Io(e) => write!(f, "{e}"),
        }
    }
}

impl From<io::Error> for PathError {
    fn from(e: io::Error) -> Self {
        if e.kind() == io::ErrorKind::NotFound {
            PathError::NotFound
        } else {
            PathError::Io(e.to_string())
        }
    }
}

const RESERVED: [&str; 22] = [
    "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2",
    "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
];

/// Checks one path segment (a file or folder name).
pub fn check_segment(seg: &str) -> Result<(), PathError> {
    let bad = |why: &str| Err(PathError::Invalid(format!("{why}: {seg:?}")));
    if seg.is_empty() {
        return bad("empty segment");
    }
    if seg == "." || seg == ".." {
        return bad("dot segment");
    }
    if seg.len() > 255 {
        return bad("name too long");
    }
    if let Some(c) = seg.chars().find(|c| (*c as u32) < 0x20 || "<>:\"|?*\\/".contains(*c)) {
        return Err(PathError::Invalid(format!("character {c:?} not allowed in {seg:?}")));
    }
    if seg.ends_with('.') || seg.ends_with(' ') {
        return bad("trailing dot or space");
    }
    let stem = seg.split('.').next().unwrap_or("").trim_end().to_ascii_uppercase();
    if RESERVED.contains(&stem.as_str()) {
        return bad("reserved device name");
    }
    Ok(())
}

/// Validates a decoded relative path and returns it in canonical form
/// (`a/b/c`, no trailing slash). The empty string means the root.
pub fn clean_rel(input: &str) -> Result<String, PathError> {
    if input.contains('\0') {
        return Err(PathError::Invalid("NUL byte".into()));
    }
    if input.contains('\\') {
        return Err(PathError::Invalid("backslash".into()));
    }
    if input.starts_with('/') {
        return Err(PathError::Invalid("absolute path".into()));
    }
    let trimmed = input.strip_suffix('/').unwrap_or(input);
    if trimmed.is_empty() {
        return Ok(String::new());
    }
    for seg in trimmed.split('/') {
        check_segment(seg)?;
    }
    Ok(trimmed.to_string())
}

/// Canonical form of the share root; resolve it once at startup.
pub fn canonical_root(root: &Path) -> io::Result<PathBuf> {
    let c = root.canonicalize()?;
    if !c.is_dir() {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, format!("{} is not a folder", root.display())));
    }
    Ok(c)
}

/// Resolves a cleaned relative path to an existing file or folder, following
/// symlinks only while they stay inside the root.
pub fn resolve_existing(root: &Path, rel: &str) -> Result<PathBuf, PathError> {
    let rel = clean_rel(rel)?;
    let mut p = root.to_path_buf();
    for seg in rel.split('/').filter(|s| !s.is_empty()) {
        p.push(seg);
    }
    let canon = p.canonicalize()?;
    if !canon.starts_with(root) {
        return Err(PathError::Outside);
    }
    Ok(canon)
}

/// Resolves a cleaned relative path for writing: creates missing parent
/// folders, refuses to pass through or replace a symlink, and returns the
/// target path (which may not exist yet).
pub fn resolve_for_write(root: &Path, rel: &str) -> Result<PathBuf, PathError> {
    let rel = clean_rel(rel)?;
    if rel.is_empty() {
        return Err(PathError::Invalid("cannot write the root".into()));
    }
    let segs: Vec<&str> = rel.split('/').collect();
    let mut p = root.to_path_buf();
    for seg in &segs[..segs.len() - 1] {
        p.push(seg);
        match std::fs::symlink_metadata(&p) {
            Ok(m) if m.file_type().is_symlink() => return Err(PathError::Outside),
            Ok(m) if m.is_dir() => {}
            Ok(_) => return Err(PathError::Invalid(format!("{seg} is a file, not a folder"))),
            Err(e) if e.kind() == io::ErrorKind::NotFound => std::fs::create_dir(&p).or_else(|e| {
                // another request may have created it a moment ago
                if e.kind() == io::ErrorKind::AlreadyExists && p.is_dir() {
                    Ok(())
                } else {
                    Err(e)
                }
            })?,
            Err(e) => return Err(e.into()),
        }
    }
    p.push(segs[segs.len() - 1]);
    if let Ok(m) = std::fs::symlink_metadata(&p) {
        if m.file_type().is_symlink() {
            return Err(PathError::Outside);
        }
    }
    // belt and braces: the parent must canonicalise inside the root
    let parent = p.parent().unwrap_or(root).canonicalize()?;
    if !parent.starts_with(root) {
        return Err(PathError::Outside);
    }
    Ok(p)
}

/// Joins a cleaned relative path onto the root without touching the disk.
pub fn join(root: &Path, rel: &str) -> PathBuf {
    let mut p = root.to_path_buf();
    for seg in rel.split('/').filter(|s| !s.is_empty()) {
        p.push(seg);
    }
    p
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("achclwss-share-paths-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn rejects_traversal_and_tricks() {
        let attacks = [
            "..",
            "../etc/passwd",
            "a/../../b",
            "a/./b",
            "/etc/passwd",
            "//server/share",
            "\\\\server\\share",
            "..\\..\\windows",
            "C:/Windows",
            "C:",
            "c:foo",
            "a//b",
            "a\0b",
            "file.txt:stream",
            "CON",
            "aux.txt",
            "Nul",
            "com1.log",
            "LPT9",
            "trail.",
            "trail ",
            "dir/./x",
            "a\nb",
            "what?",
            "star*",
            "pipe|",
            "<x>",
            "q\"",
        ];
        for a in attacks {
            assert!(clean_rel(a).is_err(), "accepted {a:?}");
        }
    }

    #[test]
    fn accepts_normal_names() {
        for ok in ["", "a", "a/b.txt", "dir/", "src/main.rs", "My Photos/IMG 001.JPG", "日本語/ファイル.txt", ".gitignore", "a..b", "...x", "console.log", "con-fig"] {
            assert!(clean_rel(ok).is_ok(), "rejected {ok:?}");
        }
        assert_eq!(clean_rel("dir/").unwrap(), "dir");
    }

    #[test]
    fn resolve_stays_inside_root() {
        let base = tmp("resolve");
        let root = base.join("root");
        std::fs::create_dir_all(root.join("sub")).unwrap();
        std::fs::write(root.join("sub/f.txt"), "x").unwrap();
        std::fs::write(base.join("secret.txt"), "s").unwrap();
        let root = canonical_root(&root).unwrap();
        assert!(resolve_existing(&root, "sub/f.txt").unwrap().ends_with("f.txt"));
        assert_eq!(resolve_existing(&root, "nope"), Err(PathError::NotFound));
        assert!(resolve_existing(&root, "../secret.txt").is_err());
        assert!(resolve_for_write(&root, "../secret.txt").is_err());
        assert!(resolve_for_write(&root, "").is_err());
        let w = resolve_for_write(&root, "new/deep/file.txt").unwrap();
        assert!(root.join("new/deep").is_dir());
        assert!(w.starts_with(&root));
        // a file in the way of a folder
        assert!(resolve_for_write(&root, "sub/f.txt/x").is_err());
        let _ = std::fs::remove_dir_all(&base);
    }

    #[cfg(unix)]
    #[test]
    fn symlinks_cannot_escape() {
        let base = tmp("symlink");
        let root = base.join("root");
        let outside = base.join("outside");
        std::fs::create_dir_all(&root).unwrap();
        std::fs::create_dir_all(&outside).unwrap();
        std::fs::write(outside.join("secret.txt"), "s").unwrap();
        std::fs::write(root.join("inside.txt"), "i").unwrap();
        std::os::unix::fs::symlink(&outside, root.join("escape")).unwrap();
        std::os::unix::fs::symlink(outside.join("secret.txt"), root.join("link.txt")).unwrap();
        std::os::unix::fs::symlink(root.join("inside.txt"), root.join("ok.txt")).unwrap();
        let root = canonical_root(&root).unwrap();
        assert_eq!(resolve_existing(&root, "escape/secret.txt"), Err(PathError::Outside));
        assert_eq!(resolve_existing(&root, "link.txt"), Err(PathError::Outside));
        assert!(resolve_existing(&root, "ok.txt").is_ok());
        assert_eq!(resolve_for_write(&root, "escape/new.txt"), Err(PathError::Outside));
        assert_eq!(resolve_for_write(&root, "link.txt"), Err(PathError::Outside));
        assert!(!outside.join("new.txt").exists());
        let _ = std::fs::remove_dir_all(&base);
    }
}
