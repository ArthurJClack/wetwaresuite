//! A folder's sync manifest: every file that isn't ignored, with its size,
//! modification time and SHA-256. Rescanning only stats files; a file is
//! re-hashed when its size or mtime moved (or it changed in the last two
//! seconds and is small, which catches edits inside one mtime tick).

use std::collections::{BTreeMap, HashSet};
use std::io;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use crate::ignore::Ignorer;
use crate::paths;
use crate::sha256;
use crate::util::unix_nanos;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileEntry {
    pub size: u64,
    pub mtime: u128,
    pub hash: String,
}

#[derive(Debug)]
pub struct Index {
    pub root: PathBuf,
    pub files: BTreeMap<String, FileEntry>,
    pub ignorer: Ignorer,
    pub include_heavy: bool,
    /// Paths a sync peer wrote here; kept in the manifest even when this
    /// side's rules would ignore them, so the peer never sees them vanish.
    pub pinned: HashSet<String>,
    /// Names that can't be shared portably (e.g. `a:b`), from the last scan.
    pub skipped: Vec<String>,
}

const RACY: Duration = Duration::from_secs(2);
const RACY_MAX_SIZE: u64 = 1 << 20;

impl Index {
    pub fn new(root: &Path, include_heavy: bool) -> Index {
        Index {
            root: root.to_path_buf(),
            files: BTreeMap::new(),
            ignorer: Ignorer::new(include_heavy),
            include_heavy,
            pinned: HashSet::new(),
            skipped: Vec::new(),
        }
    }

    fn entry_for(&self, rel: &str, meta: &std::fs::Metadata, now: SystemTime) -> io::Result<FileEntry> {
        let mtime_t = meta.modified().unwrap_or(SystemTime::UNIX_EPOCH);
        let mtime = unix_nanos(mtime_t);
        let size = meta.len();
        let racy = size <= RACY_MAX_SIZE && now.duration_since(mtime_t).map(|d| d < RACY).unwrap_or(true);
        if let Some(old) = self.files.get(rel) {
            if old.size == size && old.mtime == mtime && !racy {
                return Ok(old.clone());
            }
        }
        let hash = sha256::file_hex(&paths::join(&self.root, rel))?;
        Ok(FileEntry { size, mtime, hash })
    }

    /// Walks the tree again. Returns true when any path or hash changed.
    pub fn scan(&mut self) -> io::Result<bool> {
        let now = SystemTime::now();
        let mut ignorer = Ignorer::new(self.include_heavy);
        let mut files = BTreeMap::new();
        let mut skipped = Vec::new();
        let mut stack: Vec<(PathBuf, String)> = vec![(self.root.clone(), String::new())];
        while let Some((dir, rel)) = stack.pop() {
            ignorer.load_dir(&dir, &rel);
            let Ok(rd) = std::fs::read_dir(&dir) else { continue };
            let mut entries: Vec<_> = rd.filter_map(Result::ok).collect();
            entries.sort_by_key(|e| e.file_name());
            for e in entries {
                let Ok(ft) = e.file_type() else { continue };
                if ft.is_symlink() {
                    continue; // never follow links out of (or around) the tree
                }
                let name = e.file_name();
                let Some(name) = name.to_str() else {
                    skipped.push(format!("{rel}/{}", name.to_string_lossy()));
                    continue;
                };
                let child = if rel.is_empty() { name.to_string() } else { format!("{rel}/{name}") };
                if ignorer.ignored_entry(&child, ft.is_dir()) {
                    continue;
                }
                if paths::check_segment(name).is_err() {
                    skipped.push(child);
                    continue;
                }
                if ft.is_dir() {
                    stack.push((e.path(), child));
                } else if ft.is_file() {
                    let Ok(meta) = e.metadata() else { continue };
                    if let Ok(entry) = self.entry_for(&child, &meta, now) {
                        files.insert(child, entry);
                    }
                }
            }
        }
        let pinned: Vec<String> = self.pinned.iter().cloned().collect();
        for p in pinned {
            if files.contains_key(&p) {
                continue;
            }
            match std::fs::symlink_metadata(paths::join(&self.root, &p)) {
                Ok(meta) if meta.is_file() => {
                    if let Ok(entry) = self.entry_for(&p, &meta, now) {
                        files.insert(p, entry);
                    }
                }
                _ => {
                    self.pinned.remove(&p);
                }
            }
        }
        let changed = files.len() != self.files.len()
            || files.iter().zip(self.files.iter()).any(|((ka, a), (kb, b))| ka != kb || a.hash != b.hash);
        self.files = files;
        self.ignorer = ignorer;
        self.skipped = skipped;
        Ok(changed)
    }

    /// Re-reads one path after this process wrote or deleted it.
    pub fn refresh(&mut self, rel: &str) {
        match std::fs::symlink_metadata(paths::join(&self.root, rel)) {
            Ok(meta) if meta.is_file() => {
                if let Ok(e) = self.entry_for(rel, &meta, SystemTime::now()) {
                    self.files.insert(rel.to_string(), e);
                }
            }
            _ => {
                self.files.remove(rel);
            }
        }
    }

    pub fn hash_of(&self, rel: &str) -> Option<&str> {
        self.files.get(rel).map(|e| e.hash.as_str())
    }
}

/// What happened to one path between two manifests.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Change {
    Added(String),
    Modified(String),
    Removed(String),
}

/// Diffs two `path -> hash` maps, in path order.
pub fn diff(old: &BTreeMap<String, String>, new: &BTreeMap<String, String>) -> Vec<Change> {
    let mut out = Vec::new();
    for (p, h) in new {
        match old.get(p) {
            None => out.push(Change::Added(p.clone())),
            Some(o) if o != h => out.push(Change::Modified(p.clone())),
            _ => {}
        }
    }
    for p in old.keys() {
        if !new.contains_key(p) {
            out.push(Change::Removed(p.clone()));
        }
    }
    out.sort_by(|a, b| {
        let key = |c: &Change| match c {
            Change::Added(p) | Change::Modified(p) | Change::Removed(p) => p.clone(),
        };
        key(a).cmp(&key(b))
    });
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("wetwaresuite-share-index-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d.canonicalize().unwrap()
    }

    #[test]
    fn scan_respects_ignores_and_detects_changes() {
        let root = tmp("scan");
        std::fs::create_dir_all(root.join("src")).unwrap();
        std::fs::create_dir_all(root.join("node_modules/pkg")).unwrap();
        std::fs::create_dir_all(root.join("logs")).unwrap();
        std::fs::write(root.join("src/main.rs"), "fn main() {}").unwrap();
        std::fs::write(root.join("node_modules/pkg/index.js"), "x").unwrap();
        std::fs::write(root.join("logs/a.log"), "x").unwrap();
        std::fs::write(root.join(".gitignore"), "logs/\n").unwrap();
        let mut ix = Index::new(&root, false);
        assert!(ix.scan().unwrap());
        let keys: Vec<&str> = ix.files.keys().map(String::as_str).collect();
        assert_eq!(keys, vec![".gitignore", "src/main.rs"]);
        assert_eq!(ix.hash_of("src/main.rs").unwrap(), sha256::digest_hex(b"fn main() {}"));
        assert!(!ix.scan().unwrap());
        std::fs::write(root.join("src/main.rs"), "fn main() { }").unwrap();
        assert!(ix.scan().unwrap());
        std::fs::remove_file(root.join("src/main.rs")).unwrap();
        assert!(ix.scan().unwrap());
        assert_eq!(ix.files.len(), 1);
        // pinned paths survive ignore rules
        std::fs::write(root.join("logs/b.log"), "pinned").unwrap();
        ix.pinned.insert("logs/b.log".into());
        ix.scan().unwrap();
        assert!(ix.files.contains_key("logs/b.log"));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn manifest_diff() {
        let m = |pairs: &[(&str, &str)]| pairs.iter().map(|(a, b)| (a.to_string(), b.to_string())).collect();
        let old = m(&[("a", "1"), ("b", "2"), ("c", "3")]);
        let new = m(&[("a", "1"), ("b", "9"), ("d", "4")]);
        assert_eq!(
            diff(&old, &new),
            vec![Change::Modified("b".into()), Change::Removed("c".into()), Change::Added("d".into())]
        );
        assert!(diff(&old, &old).is_empty());
    }
}
