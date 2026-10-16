//! Live two-way sync between a local folder and a writable share.
//!
//! One side hosts (`sync <folder>` with no peer, or `serve --write`); the
//! other runs this engine against it. Each round the engine:
//! 1. long-polls the host's manifest (returns as soon as the host changes)
//! 2. rescans the local folder (stat only; hashes only what moved)
//! 3. three-way merges local, remote and the last synced state ("base"):
//!    - only one side changed: copy that side's change (including deletes)
//!    - both changed the same way: nothing to do
//!    - one deleted, the other edited: the edit wins
//!    - both edited differently: the host's version keeps the name and the
//!      local version is kept as `name.conflict-<host>.ext` on both sides
//!
//! Uploads and deletes carry `If-Match` with the hash this side last saw, so
//! an edit the engine hasn't seen yet is never overwritten; it shows up as a
//! conflict on the next round instead. The base is saved in the user's cache
//! folder so a restart doesn't resurrect files deleted while it was off.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant, SystemTime};

use crate::delta;
use crate::http::{Client, ReqBody, Target};
use crate::index::Index;
use crate::json::Json;
use crate::paths;
use crate::server::Logger;
use crate::sha256;
use crate::util;

#[derive(Clone)]
pub struct Options {
    pub folder: PathBuf,
    pub target: Target,
    pub include_heavy: bool,
    /// How long one long-poll waits for host changes when nothing happens
    /// locally; this is also the local change detection interval.
    pub poll: Duration,
    pub log: Option<Logger>,
    /// Used in conflict file names.
    pub host: String,
    /// Where the base state is saved; `None` keeps it in memory only.
    pub state_dir: Option<PathBuf>,
    /// Send only the changed blocks of big edited files when the peer can
    /// (see `delta`); off means always whole files.
    pub delta: bool,
}

impl Options {
    pub fn new(folder: impl Into<PathBuf>, target: Target) -> Options {
        Options {
            folder: folder.into(),
            target,
            include_heavy: false,
            poll: Duration::from_millis(250),
            log: None,
            host: util::hostname(),
            state_dir: default_state_dir(),
            delta: true,
        }
    }
}

/// The OS cache folder: %LOCALAPPDATA%, ~/Library/Caches, $XDG_CACHE_HOME or ~/.cache.
pub fn default_state_dir() -> Option<PathBuf> {
    let base = if cfg!(windows) {
        std::env::var_os("LOCALAPPDATA").map(PathBuf::from)
    } else if cfg!(target_os = "macos") {
        std::env::var_os("HOME").map(|h| PathBuf::from(h).join("Library/Caches"))
    } else {
        std::env::var_os("XDG_CACHE_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cache")))
    }?;
    Some(base.join("wetwaresuite-share").join("sync"))
}

#[derive(Debug, Default, Clone)]
pub struct Stats {
    pub uploaded: u64,
    pub downloaded: u64,
    pub deleted_here: u64,
    pub deleted_there: u64,
    pub conflicts: u64,
    pub rounds: u64,
    pub files: usize,
    pub in_sync: bool,
    pub last_error: Option<String>,
    pub last_change: Option<SystemTime>,
    pub events: VecDeque<String>,
    pub peer_host: String,
    /// Files sent or received as block deltas instead of whole.
    pub delta_files: u64,
    /// Deltas that failed their checks and were resent whole.
    pub delta_fallbacks: u64,
    /// File bytes actually sent and received (bodies, not headers).
    pub bytes_up: u64,
    pub bytes_down: u64,
}

impl Stats {
    pub fn to_json(&self) -> Json {
        Json::obj()
            .with("in_sync", self.in_sync)
            .with("files", self.files)
            .with("uploaded", self.uploaded)
            .with("downloaded", self.downloaded)
            .with("deleted_here", self.deleted_here)
            .with("deleted_there", self.deleted_there)
            .with("conflicts", self.conflicts)
            .with("rounds", self.rounds)
            .with("peer_host", self.peer_host.as_str())
            .with("delta_files", self.delta_files)
            .with("delta_fallbacks", self.delta_fallbacks)
            .with("bytes_up", self.bytes_up)
            .with("bytes_down", self.bytes_down)
            .with("last_error", self.last_error.clone())
            .with("last_change_unix", self.last_change.map(util::unix_secs))
            .with("recent", self.events.iter().map(Json::from).collect::<Vec<_>>())
    }
}

/// `src/main.rs` -> `src/main.conflict-laptop.rs`; adds -2, -3... if taken.
pub fn conflict_name(rel: &str, host: &str, exists: &dyn Fn(&str) -> bool) -> String {
    let (dir, name) = match rel.rsplit_once('/') {
        Some((d, n)) => (format!("{d}/"), n),
        None => (String::new(), rel),
    };
    let (stem, ext) = match name.rfind('.') {
        Some(i) if i > 0 => (&name[..i], &name[i..]),
        _ => (name, ""),
    };
    let mut n = 1;
    loop {
        let suffix = if n == 1 { String::new() } else { format!("-{n}") };
        let candidate = format!("{dir}{stem}.conflict-{host}{suffix}{ext}");
        if !exists(&candidate) {
            return candidate;
        }
        n += 1;
    }
}

/// What to do with one path, from the local, remote and base hashes.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Plan {
    Nothing,
    Pull,
    Push,
    DeleteHere,
    DeleteThere,
    Conflict,
}

pub fn plan(local: Option<&str>, remote: Option<&str>, base: Option<&str>) -> Plan {
    if local == remote {
        Plan::Nothing
    } else if local == base {
        if remote.is_none() {
            Plan::DeleteHere
        } else {
            Plan::Pull
        }
    } else if remote == base {
        if local.is_none() {
            Plan::DeleteThere
        } else {
            Plan::Push
        }
    } else if local.is_none() {
        Plan::Pull // deleted here, edited there: keep the edit
    } else if remote.is_none() {
        Plan::Push
    } else {
        Plan::Conflict
    }
}

pub struct Engine {
    opts: Options,
    local: Index,
    base: BTreeMap<String, String>,
    remote: BTreeMap<String, String>,
    remote_version: u64,
    client: Client,
    pub stats: Arc<Mutex<Stats>>,
    state_file: Option<PathBuf>,
    peer_key: String,
    /// The host advertised block deltas (`"delta": 1` in /api/info).
    peer_delta: bool,
}

impl Engine {
    pub fn new(opts: Options) -> io::Result<Engine> {
        std::fs::create_dir_all(&opts.folder)?;
        let root = paths::canonical_root(&opts.folder)?;
        let mut client = Client::new(opts.target.clone());
        client.timeout = Duration::from_secs(60);
        let info = client.fetch_json("GET", "/api/info", ReqBody::None)?;
        if !info.bool_or("writable", false) {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "that share is read-only; the other machine should run `wetwaresuite-share sync <folder>` (or serve --write)",
            ));
        }
        let peer_key = format!("{}/{}", info.str_or("host", "?"), info.str_or("folder", "?"));
        let state_file = opts.state_dir.as_ref().map(|d| {
            let key = sha256::digest_hex(format!("{}\n{peer_key}", root.display()).as_bytes());
            d.join(format!("{}.json", &key[..24]))
        });
        let peer_delta = opts.delta && info.get("delta").and_then(Json::as_u64).unwrap_or(0) >= 1;
        let mut engine = Engine {
            local: Index::new(&root, opts.include_heavy),
            opts,
            base: BTreeMap::new(),
            remote: BTreeMap::new(),
            remote_version: 0,
            client,
            stats: Arc::new(Mutex::new(Stats { peer_host: info.str_or("host", "?").to_string(), ..Default::default() })),
            state_file,
            peer_key,
            peer_delta,
        };
        engine.load_state();
        Ok(engine)
    }

    fn load_state(&mut self) {
        let Some(f) = &self.state_file else { return };
        let Ok(text) = std::fs::read_to_string(f) else { return };
        let Ok(j) = Json::parse(&text) else { return };
        if j.str_or("peer", "") != self.peer_key {
            return;
        }
        if let Some(Json::Obj(items)) = j.get("base") {
            for (k, v) in items {
                if let (Ok(k), Some(h)) = (paths::clean_rel(k), v.as_str()) {
                    self.base.insert(k, h.to_string());
                }
            }
        }
    }

    fn save_state(&self) {
        let Some(f) = &self.state_file else { return };
        let base = Json::Obj(self.base.iter().map(|(k, v)| (k.clone(), Json::from(v.as_str()))).collect());
        let j = Json::obj()
            .with("peer", self.peer_key.as_str())
            .with("folder", self.local.root.to_string_lossy().as_ref())
            .with("base", base);
        if let Some(d) = f.parent() {
            let _ = std::fs::create_dir_all(d);
        }
        let tmp = f.with_extension("tmp");
        if std::fs::write(&tmp, j.dump()).is_ok() {
            let _ = std::fs::rename(&tmp, f);
        }
    }

    fn event(&self, msg: String) {
        if let Some(l) = &self.opts.log {
            l(&msg);
        }
        let mut s = self.stats.lock().unwrap();
        s.events.push_back(msg);
        while s.events.len() > 50 {
            s.events.pop_front();
        }
        s.last_change = Some(SystemTime::now());
    }

    fn fetch_manifest(&mut self, wait: Duration) -> io::Result<bool> {
        let target = format!("/api/sync/manifest?since={}&wait={}", self.remote_version, wait.as_millis());
        let j = self.client.fetch_json("GET", &target, ReqBody::None)?;
        self.remote_version = j.get("version").and_then(Json::as_u64).unwrap_or(0);
        if j.bool_or("same", false) {
            return Ok(false);
        }
        let mut remote = BTreeMap::new();
        for item in j.get("files").and_then(Json::as_array).unwrap_or(&[]) {
            let Some([p, h, ..]) = item.as_array() else { continue };
            let (Some(p), Some(h)) = (p.as_str(), h.as_str()) else { continue };
            // a host can't make us write outside the folder
            if let Ok(clean) = paths::clean_rel(p) {
                if !clean.is_empty() {
                    remote.insert(clean, h.to_string());
                }
            }
        }
        self.remote = remote;
        Ok(true)
    }

    /// True when the local file still matches what the last scan saw.
    fn local_unchanged(&self, rel: &str) -> bool {
        let path = paths::join(&self.local.root, rel);
        match (self.local.files.get(rel), std::fs::symlink_metadata(&path)) {
            (None, Err(_)) => true,
            (Some(e), Ok(m)) if m.is_file() => {
                m.len() == e.size && m.modified().map(util::unix_nanos).unwrap_or(0) == e.mtime
            }
            _ => false,
        }
    }

    fn pull(&mut self, rel: &str) -> io::Result<()> {
        let dest = paths::resolve_for_write(&self.local.root, rel).map_err(|e| io::Error::other(e.to_string()))?;
        if dest.is_dir() {
            return Err(io::Error::other(format!("{rel} is a folder here but a file there")));
        }
        // download into a temp file first, then swap it in only if the local file didn't move meanwhile
        let dir = dest.parent().unwrap_or(&self.local.root).to_path_buf();
        let tmp = dir.join(format!("{}sync-{}", crate::ignore::TEMP_PREFIX, sha256::hex(&crate::rand::bytes::<6>()?)));
        let n = match self.pull_delta(rel, &tmp) {
            Some(n) => n,
            None => {
                let n = crate::client::download(&mut self.client, rel, &tmp, None)?;
                self.stats.lock().unwrap().bytes_down += n;
                n
            }
        };
        if !self.local_unchanged(rel) {
            let _ = std::fs::remove_file(&tmp);
            return Ok(()); // edited here mid-transfer; the next round sorts it out
        }
        let hash = sha256::file_hex(&tmp)?;
        std::fs::rename(&tmp, &dest).inspect_err(|_| {
            let _ = std::fs::remove_file(&tmp);
        })?;
        self.local.refresh(rel);
        self.base.insert(rel.to_string(), hash);
        self.stats.lock().unwrap().downloaded += 1;
        self.event(format!("↓ {rel} ({})", util::human_size(n)));
        Ok(())
    }

    /// True when a file of this size is worth a delta with this peer.
    fn delta_worthy(&self, size: u64) -> bool {
        self.peer_delta && (delta::MIN_SIZE..=delta::MAX_SIZE).contains(&size)
    }

    fn note_fallback(&self, rel: &str, why: &str) {
        self.stats.lock().unwrap().delta_fallbacks += 1;
        self.event(format!("delta for {rel} failed ({why}); sending the whole file"));
    }

    /// Downloads `rel` into `tmp` as a delta against our copy. `None` means
    /// "do a whole download instead" (no copy here, old host, any error).
    fn pull_delta(&mut self, rel: &str, tmp: &Path) -> Option<u64> {
        let basis = paths::join(&self.local.root, rel);
        let size = std::fs::metadata(&basis).ok().filter(|m| m.is_file())?.len();
        if !self.delta_worthy(size) {
            return None;
        }
        let sig = delta::Signature::of_file(&basis).ok()?.encode();
        let mut d = Vec::new();
        let target = format!("/api/sync/delta?path={}", util::percent_encode(rel));
        let resp = self.client.call("POST", &target, &[("X-Sync", "1")], ReqBody::Bytes(&sig), &mut d);
        match resp {
            Ok(r) if r.status == 200 => {}
            Ok(_) => return None, // too big there, gone, ...: whole file
            Err(_) => return None,
        }
        {
            let mut s = self.stats.lock().unwrap();
            s.bytes_up += sig.len() as u64;
            s.bytes_down += d.len() as u64;
        }
        delta::maybe_corrupt(&mut d);
        match delta::apply_to_file(&basis, &d, tmp) {
            Ok(_) => {
                self.stats.lock().unwrap().delta_files += 1;
                std::fs::metadata(tmp).ok().map(|m| m.len())
            }
            Err(e) => {
                self.note_fallback(rel, &e.to_string());
                None
            }
        }
    }

    /// Uploads `rel` as a delta against the host's copy, which must still be
    /// `if_match`. `None` means "do a whole upload instead".
    fn push_delta(&mut self, rel: &str, path: &Path, size: u64, if_match: &str) -> Option<io::Result<(u16, Vec<u8>)>> {
        if !self.delta_worthy(size) {
            return None;
        }
        let enc = util::percent_encode(rel);
        let mut sig = Vec::new();
        let r = self.client.call("GET", &format!("/api/sync/sig?path={enc}"), &[], ReqBody::None, &mut sig).ok()?;
        if r.status != 200 || r.header("x-hash") != Some(if_match) {
            return None; // changed there since the manifest: let the normal path report it
        }
        let sig = delta::Signature::decode(&sig).ok()?;
        let data = std::fs::read(path).ok()?;
        let mut d = delta::delta(&sig, &data);
        if d.len() as u64 >= size {
            return None; // nothing in common: the whole file is no bigger
        }
        delta::maybe_corrupt(&mut d);
        let mut out = Vec::new();
        let target = format!("/api/sync/patch?path={enc}");
        let headers = [("If-Match", if_match), ("X-Sync", "1")];
        let resp = match self.client.call("PUT", &target, &headers, ReqBody::Bytes(&d), &mut out) {
            Ok(r) => r,
            Err(e) => return Some(Err(e)),
        };
        {
            let mut s = self.stats.lock().unwrap();
            s.bytes_down += sig.blocks.len() as u64 * 36 + 20;
            s.bytes_up += d.len() as u64;
        }
        match resp.status {
            200 | 201 => {
                self.stats.lock().unwrap().delta_files += 1;
                Some(Ok((resp.status, out)))
            }
            412 => Some(Ok((412, out))),
            _ => {
                self.note_fallback(rel, &crate::http::http_error(resp.status, &out).to_string());
                None
            }
        }
    }

    fn push(&mut self, rel: &str, if_match: Option<&str>) -> io::Result<bool> {
        let path = paths::join(&self.local.root, rel);
        let size = std::fs::metadata(&path)?.len();
        let im = if_match.unwrap_or("none").to_string();
        let delta_result = match if_match {
            Some(h) => self.push_delta(rel, &path, size, h),
            None => None,
        };
        let (status, out) = match delta_result {
            Some(r) => r?,
            None => {
                let mut out = Vec::new();
                let resp = self.client.call(
                    "PUT",
                    &format!("/f/{}", util::encode_path(rel)),
                    &[("If-Match", &im), ("X-Sync", "1")],
                    ReqBody::File(&path),
                    &mut out,
                )?;
                self.stats.lock().unwrap().bytes_up += size;
                (resp.status, out)
            }
        };
        match status {
            201 | 200 => {
                let j = Json::parse(&String::from_utf8_lossy(&out)).unwrap_or(Json::Null);
                let hash = j.str_or("hash", "").to_string();
                self.remote.insert(rel.to_string(), hash.clone());
                self.base.insert(rel.to_string(), hash);
                self.stats.lock().unwrap().uploaded += 1;
                self.event(format!("↑ {rel} ({})", util::human_size(size)));
                Ok(true)
            }
            412 => Ok(false), // changed there since we looked; merge next round
            s => Err(crate::http::http_error(s, &out)),
        }
    }

    fn delete_there(&mut self, rel: &str, if_match: &str) -> io::Result<()> {
        let mut out = Vec::new();
        let resp = self.client.call(
            "DELETE",
            &format!("/f/{}", util::encode_path(rel)),
            &[("If-Match", if_match), ("X-Sync", "1")],
            ReqBody::None,
            &mut out,
        )?;
        match resp.status {
            200 | 404 => {
                self.remote.remove(rel);
                self.base.remove(rel);
                self.stats.lock().unwrap().deleted_there += 1;
                self.event(format!("✗ {rel} (deleted there)"));
                Ok(())
            }
            412 => Ok(()),
            s => Err(crate::http::http_error(s, &out)),
        }
    }

    fn delete_here(&mut self, rel: &str) -> io::Result<()> {
        if !self.local_unchanged(rel) {
            return Ok(());
        }
        let path = paths::join(&self.local.root, rel);
        match std::fs::remove_file(&path) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            Err(e) => return Err(e),
        }
        let mut dir = path.parent().map(Path::to_path_buf);
        while let Some(d) = dir {
            if d == self.local.root || !d.starts_with(&self.local.root) || std::fs::remove_dir(&d).is_err() {
                break;
            }
            dir = d.parent().map(Path::to_path_buf);
        }
        self.local.refresh(rel);
        self.base.remove(rel);
        self.stats.lock().unwrap().deleted_here += 1;
        self.event(format!("✗ {rel} (deleted here)"));
        Ok(())
    }

    fn conflict(&mut self, rel: &str) -> io::Result<()> {
        if !self.local_unchanged(rel) {
            return Ok(());
        }
        let root = self.local.root.clone();
        let remote = self.remote.clone();
        let exists = |p: &str| paths::join(&root, p).exists() || remote.contains_key(p);
        let copy = conflict_name(rel, &self.opts.host, &exists);
        std::fs::rename(paths::join(&root, rel), paths::join(&root, &copy))?;
        self.local.refresh(rel);
        self.local.refresh(&copy);
        let peer = {
            let mut s = self.stats.lock().unwrap();
            s.conflicts += 1;
            s.peer_host.clone()
        };
        self.event(format!("! conflict on {rel}: kept {peer}'s version, saved this one as {copy}"));
        self.pull(rel)?;
        self.push(&copy, None)?;
        Ok(())
    }

    /// One sync round. Returns how many changes it made.
    pub fn round(&mut self, wait: Duration) -> io::Result<usize> {
        let remote_changed = self.fetch_manifest(wait)?;
        let local_changed = self.local.scan()?;
        self.stats.lock().unwrap().rounds += 1;
        if !remote_changed && !local_changed && self.stats.lock().unwrap().in_sync {
            return Ok(0);
        }
        let ignorer = self.local.ignorer.clone();
        let paths_all: BTreeSet<String> =
            self.local.files.keys().chain(self.remote.keys()).chain(self.base.keys()).cloned().collect();
        let mut done = 0;
        let mut pending = 0;
        for rel in paths_all {
            if ignorer.is_ignored(&rel, false) {
                // out of scope on this side: forget it rather than "delete" it
                self.base.remove(&rel);
                continue;
            }
            let l = self.local.hash_of(&rel).map(str::to_string);
            let r = self.remote.get(&rel).cloned();
            let b = self.base.get(&rel).cloned();
            let step = plan(l.as_deref(), r.as_deref(), b.as_deref());
            let res = match step {
                Plan::Nothing => {
                    match &l {
                        Some(h) => self.base.insert(rel.clone(), h.clone()),
                        None => self.base.remove(&rel),
                    };
                    continue;
                }
                Plan::Pull => self.pull(&rel),
                // If-Match is what we last saw there: the base when only we changed, "none" when it's gone
                Plan::Push => self.push(&rel, r.as_deref()).map(|ok| {
                    if !ok {
                        pending += 1;
                    }
                }),
                Plan::DeleteHere => self.delete_here(&rel),
                Plan::DeleteThere => self.delete_there(&rel, b.as_deref().unwrap_or("none")),
                Plan::Conflict => self.conflict(&rel),
            };
            match res {
                Ok(()) => done += 1,
                Err(e) if e.kind() == io::ErrorKind::NotFound => pending += 1, // vanished mid-round
                Err(e) if is_network(&e) => return Err(e),
                Err(e) => {
                    pending += 1;
                    self.event(format!("could not sync {rel}: {e}"));
                }
            }
        }
        {
            let mut s = self.stats.lock().unwrap();
            s.files = self.local.files.len();
            // a round that changed things is confirmed by the next one
            s.in_sync = pending == 0 && done == 0;
        }
        if done > 0 {
            self.save_state();
        }
        Ok(done)
    }
}

fn is_network(e: &io::Error) -> bool {
    matches!(
        e.kind(),
        io::ErrorKind::ConnectionRefused
            | io::ErrorKind::ConnectionReset
            | io::ErrorKind::ConnectionAborted
            | io::ErrorKind::BrokenPipe
            | io::ErrorKind::TimedOut
            | io::ErrorKind::UnexpectedEof
            | io::ErrorKind::PermissionDenied
    )
}

pub struct SyncHandle {
    stop: Arc<AtomicBool>,
    pub stats: Arc<Mutex<Stats>>,
    pub folder: PathBuf,
    pub peer: String,
    thread: Option<JoinHandle<()>>,
}

impl SyncHandle {
    pub fn stop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }

    pub fn running(&self) -> bool {
        self.thread.as_ref().is_some_and(|t| !t.is_finished())
    }

    /// Blocks until the sync thread exits (it runs until stopped).
    pub fn wait(&mut self) {
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Drop for SyncHandle {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Connects, then syncs on a background thread until stopped.
pub fn start(opts: Options) -> io::Result<SyncHandle> {
    let mut engine = Engine::new(opts.clone())?;
    let stats = engine.stats.clone();
    let stop = Arc::new(AtomicBool::new(false));
    let flag = stop.clone();
    let poll = opts.poll;
    let log = opts.log.clone();
    let thread = std::thread::spawn(move || {
        let mut wait = Duration::ZERO;
        let mut failing_since: Option<Instant> = None;
        while !flag.load(Ordering::Relaxed) {
            match engine.round(wait) {
                Ok(n) => {
                    if failing_since.take().is_some() {
                        if let Some(l) = &log {
                            l("reconnected");
                        }
                    }
                    engine.stats.lock().unwrap().last_error = None;
                    // after changes, go again at once to settle; otherwise wait for news
                    wait = if n > 0 { Duration::ZERO } else { poll };
                }
                Err(e) => {
                    let msg = e.to_string();
                    if failing_since.is_none() {
                        failing_since = Some(Instant::now());
                        if let Some(l) = &log {
                            l(&format!("sync paused: {msg} (retrying)"));
                        }
                    }
                    {
                        let mut s = engine.stats.lock().unwrap();
                        s.last_error = Some(msg);
                        s.in_sync = false;
                    }
                    for _ in 0..10 {
                        if flag.load(Ordering::Relaxed) {
                            break;
                        }
                        std::thread::sleep(Duration::from_millis(100));
                    }
                    wait = Duration::ZERO;
                }
            }
        }
    });
    Ok(SyncHandle { stop, stats, folder: opts.folder.clone(), peer: opts.target.base_url(), thread: Some(thread) })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn three_way_plans() {
        let (a, b, c) = (Some("a"), Some("b"), Some("c"));
        assert_eq!(plan(a, a, None), Plan::Nothing);
        assert_eq!(plan(a, a, b), Plan::Nothing);
        assert_eq!(plan(None, None, a), Plan::Nothing);
        // one side changed
        assert_eq!(plan(a, b, a), Plan::Pull);
        assert_eq!(plan(b, a, a), Plan::Push);
        assert_eq!(plan(None, a, None), Plan::Pull); // new there
        assert_eq!(plan(a, None, None), Plan::Push); // new here
        assert_eq!(plan(a, None, a), Plan::DeleteHere);
        assert_eq!(plan(None, a, a), Plan::DeleteThere);
        // both changed
        assert_eq!(plan(b, c, a), Plan::Conflict);
        assert_eq!(plan(a, b, None), Plan::Conflict); // first sync, different content
        assert_eq!(plan(None, b, a), Plan::Pull); // deleted here, edited there
        assert_eq!(plan(b, None, a), Plan::Push); // edited here, deleted there
    }

    #[test]
    fn conflict_names() {
        let none = |_: &str| false;
        assert_eq!(conflict_name("src/main.rs", "laptop", &none), "src/main.conflict-laptop.rs");
        assert_eq!(conflict_name("Makefile", "pc", &none), "Makefile.conflict-pc");
        assert_eq!(conflict_name(".env", "pc", &none), ".env.conflict-pc");
        assert_eq!(conflict_name("a.tar.gz", "pc", &none), "a.tar.conflict-pc.gz");
        let taken = |p: &str| p == "x.conflict-pc.txt";
        assert_eq!(conflict_name("x.txt", "pc", &taken), "x.conflict-pc-2.txt");
    }
}
