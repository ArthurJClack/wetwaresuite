//! The share server: a web UI and JSON API over one folder (or one file),
//! plus the sync endpoints. One thread per connection, keep-alive.
//!
//! Security model:
//! - every request needs the share's token: `?k=TOKEN` once (then an
//!   HttpOnly, SameSite=Strict cookie), or `Authorization: Bearer TOKEN`
//! - the 6-character pairing code is only good for `POST /api/pair`, which
//!   is rate-limited per address and switches itself off after 30 misses
//! - addresses that present wrong tokens 10 times in a minute are blocked
//!   for five minutes
//! - writes need `--write`; cookie-authenticated writes also need an
//!   `X-Share` header, which a cross-site form can't send (CSRF)
//! - every path goes through `paths` (no `..`, no absolute or drive paths,
//!   no symlinks out of the root)

use std::collections::HashMap;
use std::io::{self, BufReader, Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant, SystemTime};

use crate::http::{self, Body, ParseError, Request, Response};
use crate::index::Index;
use crate::json::Json;
use crate::paths::{self, PathError};
use crate::sha256::{self, Sha256};
use crate::util::{self, unix_secs};
use crate::zip::ZipWriter;
use crate::{discovery, rand};

pub type Logger = Arc<dyn Fn(&str) + Send + Sync>;

#[derive(Clone)]
pub struct Config {
    /// The folder (or single file) to share.
    pub path: PathBuf,
    pub writable: bool,
    pub bind: IpAddr,
    /// 0 lets the OS pick a free port.
    pub port: u16,
    /// Try the next ports when `port` is taken.
    pub port_fallback: bool,
    pub max_upload: u64,
    pub include_heavy: bool,
    pub announce: bool,
    /// Shown in announcements: share, sync, send or receive.
    pub kind: String,
    /// Fixed secrets (tests); random when `None`.
    pub token: Option<String>,
    pub code: Option<String>,
    /// Allow pairing by code at all.
    pub pairing: bool,
    pub log: Option<Logger>,
}

impl Config {
    pub fn new(path: impl Into<PathBuf>) -> Config {
        Config {
            path: path.into(),
            writable: false,
            bind: IpAddr::V4(Ipv4Addr::UNSPECIFIED),
            port: crate::DEFAULT_PORT,
            port_fallback: true,
            max_upload: 4 << 30,
            include_heavy: false,
            announce: true,
            kind: "share".into(),
            token: None,
            code: None,
            pairing: true,
            log: None,
        }
    }
}

const BLOCK_AFTER: u32 = 10;
const BLOCK_FOR: Duration = Duration::from_secs(300);
const PAIR_PER_MINUTE: u32 = 5;
const PAIR_GLOBAL_LIMIT: u32 = 30;
const MAX_CONNECTIONS: usize = 256;
const SCAN_EVERY: Duration = Duration::from_millis(150);
const SYNC_IDLE: Duration = Duration::from_secs(60);

#[derive(Default)]
struct Strikes {
    window_start: Option<Instant>,
    count: u32,
    pair_count: u32,
    blocked_until: Option<Instant>,
}

pub struct SyncHost {
    pub index: Index,
    pub version: u64,
    last_poll: Instant,
}

pub struct Shared {
    pub root: PathBuf,
    pub single: Option<String>,
    pub cfg: Config,
    pub token: String,
    pub code: String,
    pub host: String,
    pub id: String,
    stop: AtomicBool,
    strikes: Mutex<HashMap<IpAddr, Strikes>>,
    pair_failures: AtomicU32,
    pairing_open: AtomicBool,
    sync: Mutex<Option<SyncHost>>,
    sync_cv: Condvar,
    pub requests: AtomicU64,
    pub bytes_out: AtomicU64,
    pub bytes_in: AtomicU64,
    pub done_count: AtomicU32,
    active: AtomicUsize,
    pub started: Instant,
    pub addr: Mutex<Option<SocketAddr>>,
}

impl Shared {
    fn log(&self, msg: &str) {
        if let Some(l) = &self.cfg.log {
            l(msg);
        }
    }

    pub fn stopping(&self) -> bool {
        self.stop.load(Ordering::Relaxed)
    }

    pub fn cookie_name(&self) -> String {
        // per-share name so a cookie from an earlier share isn't mistaken for a bad guess
        format!("wetwaresuite_share_{}", &sha256::digest_hex(self.token.as_bytes())[..8])
    }

    /// The shared folder, or the shared file.
    pub fn shared_path(&self) -> PathBuf {
        match &self.single {
            Some(name) => self.root.join(name),
            None => self.root.clone(),
        }
    }

    pub fn folder_name(&self) -> String {
        self.single.clone().unwrap_or_else(|| {
            self.root.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "share".into())
        })
    }

    fn blocked(&self, ip: IpAddr) -> bool {
        let mut map = self.strikes.lock().unwrap();
        match map.get_mut(&ip) {
            Some(s) => match s.blocked_until {
                Some(t) if Instant::now() < t => true,
                Some(_) => {
                    *s = Strikes::default();
                    false
                }
                None => false,
            },
            None => false,
        }
    }

    /// Records a failed attempt; returns true when the address is now blocked.
    fn strike(&self, ip: IpAddr, pairing: bool) -> bool {
        let mut map = self.strikes.lock().unwrap();
        if map.len() > 10_000 {
            map.retain(|_, s| s.blocked_until.is_some());
        }
        let s = map.entry(ip).or_default();
        let now = Instant::now();
        if s.window_start.is_none_or(|w| now.duration_since(w) > Duration::from_secs(60)) {
            s.window_start = Some(now);
            s.count = 0;
            s.pair_count = 0;
        }
        if pairing {
            s.pair_count += 1;
        } else {
            s.count += 1;
        }
        if s.count >= BLOCK_AFTER || s.pair_count >= PAIR_PER_MINUTE {
            s.blocked_until = Some(now + BLOCK_FOR);
            return true;
        }
        false
    }

    fn bump_sync(&self, rels: &[&str], pin: bool) {
        let mut guard = self.sync.lock().unwrap();
        if let Some(host) = guard.as_mut() {
            for rel in rels {
                if pin {
                    host.index.pinned.insert(rel.to_string());
                }
                host.index.refresh(rel);
            }
            host.version += 1;
            self.sync_cv.notify_all();
        }
    }

    pub fn status(&self) -> Json {
        let sync = self.sync.lock().unwrap();
        Json::obj()
            .with("folder", self.shared_path().to_string_lossy().as_ref())
            .with("writable", self.cfg.writable)
            .with("kind", self.cfg.kind.as_str())
            .with("code", self.code.as_str())
            .with("requests", self.requests.load(Ordering::Relaxed))
            .with("bytes_sent", self.bytes_out.load(Ordering::Relaxed))
            .with("bytes_received", self.bytes_in.load(Ordering::Relaxed))
            .with("connections", self.active.load(Ordering::Relaxed))
            .with("downloads_finished", self.done_count.load(Ordering::Relaxed) as u64)
            .with("uptime_s", self.started.elapsed().as_secs())
            .with("sync_files", sync.as_ref().map(|s| s.index.files.len() as u64))
    }
}

pub struct Share {
    pub shared: Arc<Shared>,
    pub addr: SocketAddr,
    accept: Option<JoinHandle<()>>,
    others: Vec<JoinHandle<()>>,
}

impl Share {
    pub fn start(cfg: Config) -> io::Result<Share> {
        let meta = std::fs::metadata(&cfg.path)
            .map_err(|e| io::Error::new(e.kind(), format!("{}: {e}", cfg.path.display())))?;
        let (root, single) = if meta.is_file() {
            let canon = cfg.path.canonicalize()?;
            let name = canon
                .file_name()
                .and_then(|n| n.to_str())
                .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "file name is not UTF-8"))?
                .to_string();
            paths::check_segment(&name).map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e.to_string()))?;
            (canon.parent().unwrap_or(Path::new("/")).to_path_buf(), Some(name))
        } else {
            (paths::canonical_root(&cfg.path)?, None)
        };
        let token = match &cfg.token {
            Some(t) => t.clone(),
            None => rand::token()?,
        };
        let code = match &cfg.code {
            Some(c) => rand::normalize_code(c),
            None => rand::code()?,
        };
        let listener = bind(cfg.bind, cfg.port, cfg.port_fallback)?;
        let addr = listener.local_addr()?;
        let shared = Arc::new(Shared {
            root,
            single,
            host: util::hostname(),
            id: sha256::hex(&rand::bytes::<8>()?),
            pairing_open: AtomicBool::new(cfg.pairing),
            cfg,
            token,
            code,
            stop: AtomicBool::new(false),
            strikes: Mutex::new(HashMap::new()),
            pair_failures: AtomicU32::new(0),
            sync: Mutex::new(None),
            sync_cv: Condvar::new(),
            requests: AtomicU64::new(0),
            bytes_out: AtomicU64::new(0),
            bytes_in: AtomicU64::new(0),
            done_count: AtomicU32::new(0),
            active: AtomicUsize::new(0),
            started: Instant::now(),
            addr: Mutex::new(Some(addr)),
        });
        let mut others = Vec::new();
        if shared.cfg.announce && !addr.ip().is_loopback() {
            let ann = discovery::Announcement {
                id: shared.id.clone(),
                host: shared.host.clone(),
                folder: shared.folder_name(),
                port: addr.port(),
                writable: shared.cfg.writable,
                kind: shared.cfg.kind.clone(),
            };
            let stop = Arc::new(AtomicBool::new(false));
            let flag = stop.clone();
            let s2 = shared.clone();
            others.push(discovery::announce(ann, stop));
            // mirror the share's stop flag onto the announcer's
            others.push(std::thread::spawn(move || {
                while !s2.stopping() {
                    std::thread::sleep(Duration::from_millis(100));
                }
                flag.store(true, Ordering::Relaxed);
            }));
        }
        let s = shared.clone();
        let accept = std::thread::spawn(move || accept_loop(s, listener));
        Ok(Share { shared, addr, accept: Some(accept), others })
    }

    pub fn token(&self) -> &str {
        &self.shared.token
    }

    pub fn code(&self) -> &str {
        &self.shared.code
    }

    pub fn port(&self) -> u16 {
        self.addr.port()
    }

    /// URLs (with the token) that other machines can open.
    pub fn urls(&self) -> Vec<String> {
        let ip = self.addr.ip();
        let mut hosts: Vec<String> = Vec::new();
        if ip.is_unspecified() {
            if let Some(lan) = util::primary_lan_ip() {
                hosts.push(lan.to_string());
            }
        } else if !ip.is_loopback() {
            hosts.push(ip.to_string());
        }
        hosts.into_iter().map(|h| format!("http://{h}:{}/?k={}", self.port(), self.token())).collect()
    }

    pub fn local_url(&self) -> String {
        format!("http://127.0.0.1:{}/?k={}", self.port(), self.token())
    }

    pub fn stop(&mut self) {
        if self.shared.stop.swap(true, Ordering::Relaxed) {
            return;
        }
        self.shared.sync_cv.notify_all();
        // wake the blocking accept()
        let wake = if self.addr.ip().is_unspecified() {
            SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), self.addr.port())
        } else {
            self.addr
        };
        let _ = TcpStream::connect_timeout(&wake, Duration::from_millis(500));
        if let Some(h) = self.accept.take() {
            let _ = h.join();
        }
        for h in self.others.drain(..) {
            let _ = h.join();
        }
    }

    /// Blocks until the share is stopped (from another thread or by `stop`).
    pub fn wait(&mut self) {
        if let Some(h) = self.accept.take() {
            let _ = h.join();
        }
    }
}

impl Drop for Share {
    fn drop(&mut self) {
        self.stop();
    }
}

fn bind(ip: IpAddr, port: u16, fallback: bool) -> io::Result<TcpListener> {
    let tries = if fallback && port != 0 { 20 } else { 1 };
    let mut last = None;
    for i in 0..tries {
        match TcpListener::bind((ip, port.saturating_add(i))) {
            Ok(l) => return Ok(l),
            Err(e) if e.kind() == io::ErrorKind::AddrInUse => last = Some(e),
            Err(e) => return Err(e),
        }
    }
    Err(last.unwrap_or_else(|| io::Error::new(io::ErrorKind::AddrInUse, "no free port")))
}

fn accept_loop(shared: Arc<Shared>, listener: TcpListener) {
    for stream in listener.incoming() {
        if shared.stopping() {
            break;
        }
        let Ok(stream) = stream else { continue };
        if shared.active.load(Ordering::Relaxed) >= MAX_CONNECTIONS {
            let _ = http::write_response(&stream, Response::error(503, "too many connections"), false, false);
            continue;
        }
        shared.active.fetch_add(1, Ordering::Relaxed);
        let s = shared.clone();
        std::thread::spawn(move || {
            serve_conn(&s, stream);
            s.active.fetch_sub(1, Ordering::Relaxed);
        });
    }
}

fn serve_conn(shared: &Arc<Shared>, stream: TcpStream) {
    let Ok(peer) = stream.peer_addr() else { return };
    let _ = stream.set_nodelay(true);
    let _ = stream.set_read_timeout(Some(Duration::from_secs(120)));
    let _ = stream.set_write_timeout(Some(Duration::from_secs(120)));
    let Ok(read_half) = stream.try_clone() else { return };
    let mut reader = BufReader::with_capacity(64 * 1024, read_half);
    loop {
        let req = match http::read_request(&mut reader, peer) {
            Ok(Some(r)) => r,
            Ok(None) | Err(ParseError::Io(_)) => return,
            Err(ParseError::Bad(why)) => {
                let _ = http::write_response(&stream, Response::error(400, why), false, false);
                return;
            }
            Err(ParseError::TooLarge) => {
                let _ = http::write_response(&stream, Response::error(400, "request head too large"), false, false);
                return;
            }
        };
        shared.requests.fetch_add(1, Ordering::Relaxed);
        if req.header("expect").is_some_and(|e| e.eq_ignore_ascii_case("100-continue"))
            && req.content_length <= shared.cfg.max_upload
        {
            let mut s = &stream;
            if s.write_all(b"HTTP/1.1 100 Continue\r\n\r\n").is_err() {
                return;
            }
        }
        let mut keep = req.wants_keep_alive() && !shared.stopping();
        let mut body = (&mut reader).take(req.content_length);
        let resp = route(shared, &req, &mut body);
        let left = body.limit();
        if left > 0 {
            let drained = left <= 1 << 20 && io::copy(&mut body, &mut io::sink()).map(|n| n == left).unwrap_or(false);
            keep = keep && drained;
        }
        match http::write_response(&stream, resp, req.method == "HEAD", keep) {
            Ok(n) => {
                shared.bytes_out.fetch_add(n, Ordering::Relaxed);
            }
            Err(_) => return,
        }
        if !keep {
            // lingering close: closing with unread request bytes would send a
            // reset that can destroy the response before the client reads it
            let _ = stream.shutdown(std::net::Shutdown::Write);
            let _ = reader.get_ref().set_read_timeout(Some(Duration::from_secs(2)));
            let _ = io::copy(&mut (&mut reader).take(8 << 20), &mut io::sink());
            return;
        }
    }
}

#[derive(PartialEq)]
enum Auth {
    /// Valid token; `from_query` means set a cookie and drop it from the URL.
    Ok { cookie: bool, from_query: bool },
    Missing,
    Bad,
}

fn check_auth(shared: &Shared, req: &Request) -> Auth {
    let good = |t: &str| rand::constant_time_eq(t.as_bytes(), shared.token.as_bytes());
    if let Some(k) = req.query("k") {
        return if good(k) { Auth::Ok { cookie: false, from_query: true } } else { Auth::Bad };
    }
    if let Some(h) = req.header("authorization") {
        let t = h.strip_prefix("Bearer ").or_else(|| h.strip_prefix("bearer ")).unwrap_or("");
        return if good(t) { Auth::Ok { cookie: false, from_query: false } } else { Auth::Bad };
    }
    if let Some(c) = req.cookie(&shared.cookie_name()) {
        return if good(c) { Auth::Ok { cookie: true, from_query: false } } else { Auth::Bad };
    }
    Auth::Missing
}

fn security_headers(r: Response) -> Response {
    r.header("X-Content-Type-Options", "nosniff").header("Referrer-Policy", "no-referrer")
}

fn route(shared: &Arc<Shared>, req: &Request, body: &mut dyn Read) -> Response {
    let ip = req.peer.ip();
    if shared.blocked(ip) {
        return Response::error(429, "too many failed attempts from this address; try again in a few minutes");
    }
    if req.path == "/favicon.ico" {
        return Response::new(204);
    }
    if req.path == "/api/pair" {
        return security_headers(pair(shared, req, body));
    }
    let auth = check_auth(shared, req);
    match auth {
        Auth::Bad => {
            if shared.strike(ip, false) {
                shared.log(&format!("blocked {ip} for 5 minutes after repeated bad tokens"));
            }
            Response::error(401, "wrong or expired share token")
        }
        Auth::Missing => {
            if req.path == "/" && req.method == "GET" {
                security_headers(Response::bytes(401, "text/html; charset=utf-8", ui::LOGIN.as_bytes().to_vec()))
            } else {
                Response::error(401, "this share needs its token: open the full link (…/?k=TOKEN) or pair with the code")
            }
        }
        Auth::Ok { cookie, from_query } => {
            if from_query && req.method == "GET" && req.path == "/" {
                // swap the token in the URL for a cookie and drop it from the address bar
                return security_headers(Response::redirect("/").header("Set-Cookie", &cookie_header(shared)));
            }
            let mutating = !matches!(req.method.as_str(), "GET" | "HEAD");
            if mutating && cookie && req.header("x-share").is_none() {
                return Response::error(403, "missing X-Share header");
            }
            let mut resp = handle(shared, req, body);
            if from_query {
                resp = resp.header("Set-Cookie", &cookie_header(shared));
            }
            security_headers(resp)
        }
    }
}

fn cookie_header(shared: &Shared) -> String {
    format!("{}={}; Path=/; HttpOnly; SameSite=Strict; Max-Age=86400", shared.cookie_name(), shared.token)
}

fn perr(e: PathError) -> Response {
    let status = match e {
        PathError::NotFound => 404,
        PathError::Outside => 403,
        PathError::Invalid(_) => 400,
        PathError::Io(_) => 500,
    };
    Response::error(status, &e.to_string())
}

fn ioerr(e: io::Error) -> Response {
    let status = match e.kind() {
        io::ErrorKind::NotFound => 404,
        io::ErrorKind::PermissionDenied => 403,
        _ => 500,
    };
    Response::error(status, &e.to_string())
}

fn handle(shared: &Arc<Shared>, req: &Request, body: &mut dyn Read) -> Response {
    let m = req.method.as_str();
    let p = req.path.as_str();
    let q = |k: &str| req.query(k).unwrap_or("").to_string();
    match (m, p) {
        ("GET" | "HEAD", "/") => Response::bytes(200, "text/html; charset=utf-8", ui::PAGE.as_bytes().to_vec())
            .header("Cache-Control", "no-store")
            .header(
                "Content-Security-Policy",
                "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' data: blob:; \
                 media-src 'self'; frame-ancestors 'none'",
            ),
        ("GET", "/api/info") => Response::json(200, &info(shared)),
        ("GET", "/api/list") => list(shared, &q("path")),
        ("GET", "/api/tree") => tree(shared, &q("path")),
        ("GET", "/api/hash") => match resolve_read(shared, &q("path")) {
            Ok(path) if path.is_file() => match sha256::file_hex(&path) {
                Ok(h) => Response::json(200, &Json::obj().with("hash", h)),
                Err(e) => ioerr(e),
            },
            Ok(_) => Response::error(400, "not a file"),
            Err(e) => perr(e),
        },
        ("GET", "/api/sync/manifest") => manifest(shared, req),
        ("POST", "/api/done") => {
            let n = shared.done_count.fetch_add(1, Ordering::Relaxed) + 1;
            shared.log(&format!("{} finished a transfer ({n})", req.peer.ip()));
            Response::json(200, &Json::obj().with("ok", true))
        }
        ("POST", "/api/mkdir") => {
            if let Some(r) = deny_write(shared) {
                return r;
            }
            let rel = q("path");
            match paths::resolve_for_write(&shared.root, &rel) {
                Ok(dir) => match std::fs::create_dir(&dir) {
                    Ok(()) => Response::json(201, &Json::obj().with("path", rel)),
                    Err(e) if e.kind() == io::ErrorKind::AlreadyExists && dir.is_dir() => {
                        Response::json(200, &Json::obj().with("path", rel))
                    }
                    Err(e) => ioerr(e),
                },
                Err(e) => perr(e),
            }
        }
        ("POST", "/api/move") => {
            if let Some(r) = deny_write(shared) {
                return r;
            }
            move_entry(shared, &q("from"), &q("to"))
        }
        ("GET" | "HEAD", _) if p.starts_with("/f/") || p == "/f" => get_file(shared, req, p.strip_prefix("/f").unwrap_or("")),
        ("GET" | "HEAD", _) if p.starts_with("/zip/") || p == "/zip" => {
            get_zip(shared, p.strip_prefix("/zip").unwrap_or("").trim_start_matches('/'))
        }
        ("PUT", _) if p.starts_with("/f/") => put_file(shared, req, &p[3..], body),
        ("DELETE", _) if p.starts_with("/f/") => delete(shared, req, &p[3..]),
        _ => Response::error(if p.starts_with("/api/") || p.starts_with("/f/") { 405 } else { 404 }, "no such endpoint"),
    }
}

fn info(shared: &Shared) -> Json {
    Json::obj()
        .with("app", "wetwaresuite-share")
        .with("version", crate::VERSION)
        .with("host", shared.host.as_str())
        .with("folder", shared.folder_name())
        .with("writable", shared.cfg.writable && shared.single.is_none())
        .with("single", shared.single.is_some())
        .with("kind", shared.cfg.kind.as_str())
        .with("max_upload", shared.cfg.max_upload)
        .with("include_heavy", shared.cfg.include_heavy)
}

fn deny_write(shared: &Shared) -> Option<Response> {
    if !shared.cfg.writable || shared.single.is_some() {
        Some(Response::error(403, "this share is read-only (start it with --write to allow uploads)"))
    } else {
        None
    }
}

/// Resolves a path for reading, honouring single-file shares.
fn resolve_read(shared: &Shared, rel: &str) -> Result<PathBuf, PathError> {
    let rel = paths::clean_rel(rel)?;
    if let Some(name) = &shared.single {
        if rel != *name {
            return Err(PathError::NotFound);
        }
    }
    paths::resolve_existing(&shared.root, &rel)
}

fn entry_json(name: &str, meta: &std::fs::Metadata) -> Json {
    Json::obj()
        .with("name", name)
        .with("dir", meta.is_dir())
        .with("size", if meta.is_dir() { 0 } else { meta.len() })
        .with("mtime", meta.modified().map(unix_secs).unwrap_or(0))
}

fn list(shared: &Shared, rel: &str) -> Response {
    let rel = match paths::clean_rel(rel) {
        Ok(r) => r,
        Err(e) => return perr(e),
    };
    let mut entries = Vec::new();
    if let Some(name) = &shared.single {
        if !rel.is_empty() {
            return Response::error(404, "not found");
        }
        if let Ok(meta) = std::fs::metadata(shared.root.join(name)) {
            entries.push(entry_json(name, &meta));
        }
    } else {
        let dir = match paths::resolve_existing(&shared.root, &rel) {
            Ok(d) => d,
            Err(e) => return perr(e),
        };
        let rd = match std::fs::read_dir(&dir) {
            Ok(rd) => rd,
            Err(e) => return ioerr(e),
        };
        for e in rd.filter_map(Result::ok) {
            let Some(name) = e.file_name().to_str().map(str::to_string) else { continue };
            if name.starts_with(crate::ignore::TEMP_PREFIX) || paths::check_segment(&name).is_err() {
                continue;
            }
            let Ok(ft) = e.file_type() else { continue };
            if ft.is_symlink() {
                // show links only when they point inside the share
                match e.path().canonicalize() {
                    Ok(t) if t.starts_with(&shared.root) => {}
                    _ => continue,
                }
            }
            if let Ok(meta) = std::fs::metadata(e.path()) {
                entries.push(entry_json(&name, &meta));
            }
        }
    }
    Response::json(200, &Json::obj().with("path", rel).with("entries", entries))
}

/// Every file under a folder (no symlinks), for `receive` and `fetch_remote`.
fn walk_files(dir: &Path, prefix: &str, out: &mut Vec<(String, std::fs::Metadata)>, limit: usize) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    let mut entries: Vec<_> = rd.filter_map(Result::ok).collect();
    entries.sort_by_key(|e| e.file_name());
    for e in entries {
        if out.len() >= limit {
            return;
        }
        let Some(name) = e.file_name().to_str().map(str::to_string) else { continue };
        if name.starts_with(crate::ignore::TEMP_PREFIX) || paths::check_segment(&name).is_err() {
            continue;
        }
        let Ok(ft) = e.file_type() else { continue };
        let rel = if prefix.is_empty() { name.clone() } else { format!("{prefix}/{name}") };
        if ft.is_dir() {
            walk_files(&e.path(), &rel, out, limit);
        } else if ft.is_file() {
            if let Ok(m) = e.metadata() {
                out.push((rel, m));
            }
        }
    }
}

fn tree(shared: &Shared, rel: &str) -> Response {
    let mut files = Vec::new();
    if let Some(name) = &shared.single {
        if let Ok(m) = std::fs::metadata(shared.root.join(name)) {
            files.push((name.clone(), m));
        }
    } else {
        let dir = match paths::resolve_existing(&shared.root, rel) {
            Ok(d) => d,
            Err(e) => return perr(e),
        };
        let base = paths::clean_rel(rel).unwrap_or_default();
        if dir.is_file() {
            if let Ok(m) = std::fs::metadata(&dir) {
                files.push((base.clone(), m));
            }
        } else {
            walk_files(&dir, &base, &mut files, 500_000);
        }
    }
    let list: Vec<Json> = files
        .iter()
        .map(|(p, m)| {
            Json::obj()
                .with("path", p.as_str())
                .with("size", m.len())
                .with("mtime", m.modified().map(unix_secs).unwrap_or(0))
        })
        .collect();
    Response::json(200, &Json::obj().with("files", list))
}

pub fn mime_for(name: &str) -> &'static str {
    let ext = name.rsplit_once('.').map(|(_, e)| e.to_ascii_lowercase()).unwrap_or_default();
    match ext.as_str() {
        "html" | "htm" => "text/html; charset=utf-8",
        "txt" | "md" | "rs" | "c" | "h" | "cpp" | "hpp" | "py" | "js" | "ts" | "toml" | "yaml" | "yml" | "ini"
        | "cfg" | "log" | "sh" | "go" | "java" | "kt" | "cmake" | "csv" | "lock" => "text/plain; charset=utf-8",
        "css" => "text/css; charset=utf-8",
        "json" => "application/json",
        "svg" => "image/svg+xml",
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "webp" => "image/webp",
        "avif" => "image/avif",
        "bmp" => "image/bmp",
        "ico" => "image/x-icon",
        "mp4" | "m4v" => "video/mp4",
        "webm" => "video/webm",
        "mov" => "video/quicktime",
        "mp3" => "audio/mpeg",
        "wav" => "audio/wav",
        "ogg" | "oga" => "audio/ogg",
        "flac" => "audio/flac",
        "m4a" => "audio/mp4",
        "pdf" => "application/pdf",
        "zip" => "application/zip",
        "xml" => "application/xml",
        _ => "application/octet-stream",
    }
}

fn disposition(kind: &str, name: &str) -> String {
    let ascii: String = name.chars().map(|c| if c.is_ascii_graphic() && c != '"' && c != '\\' || c == ' ' { c } else { '_' }).collect();
    format!("{kind}; filename=\"{ascii}\"; filename*=UTF-8''{}", util::percent_encode(name))
}

fn get_file(shared: &Shared, req: &Request, rel: &str) -> Response {
    let rel = rel.trim_start_matches('/');
    let path = match resolve_read(shared, rel) {
        Ok(p) => p,
        Err(e) => return perr(e),
    };
    if path.is_dir() {
        return Response::redirect(&format!("/#/{}", util::encode_path(rel)));
    }
    let file = match std::fs::File::open(&path) {
        Ok(f) => f,
        Err(e) => return ioerr(e),
    };
    let len = match file.metadata() {
        Ok(m) => m.len(),
        Err(e) => return ioerr(e),
    };
    let name = rel.rsplit('/').next().unwrap_or(rel);
    let mime = mime_for(name);
    let kind = if req.query("dl").is_some() { "attachment" } else { "inline" };
    let mut resp = Response::new(200)
        .header("Content-Type", mime)
        .header("Accept-Ranges", "bytes")
        .header("Cache-Control", "no-cache")
        .header("Content-Disposition", &disposition(kind, name))
        // shared files never run script on the share's origin
        .header("Content-Security-Policy", "sandbox");
    let (start, n) = match req.header("range").and_then(|r| http::parse_range(r, len)) {
        Some(Ok((s, n))) => {
            resp.status = 206;
            resp = resp.header("Content-Range", &format!("bytes {s}-{}/{len}", s + n - 1));
            (s, n)
        }
        Some(Err(())) => return Response::new(416).header("Content-Range", &format!("bytes */{len}")),
        None => (0, len),
    };
    resp.with_body(Body::File(file, start, n))
}

fn get_zip(shared: &Shared, rel: &str) -> Response {
    let (dir, base_name, files) = if let Some(name) = &shared.single {
        let p = shared.root.join(name);
        let m = match std::fs::metadata(&p) {
            Ok(m) => m,
            Err(e) => return ioerr(e),
        };
        (shared.root.clone(), name.clone(), vec![(name.clone(), m)])
    } else {
        let dir = match paths::resolve_existing(&shared.root, rel) {
            Ok(d) => d,
            Err(e) => return perr(e),
        };
        if !dir.is_dir() {
            return Response::error(400, "not a folder");
        }
        let name = if rel.is_empty() { shared.folder_name() } else { rel.rsplit('/').next().unwrap_or(rel).to_string() };
        let mut files = Vec::new();
        walk_files(&dir, "", &mut files, 1_000_000);
        (dir, name, files)
    };
    let prefix = base_name.clone();
    let single = shared.single.is_some();
    let stream: http::StreamFn = Box::new(move |out: &mut dyn Write| {
        let mut z = ZipWriter::new(out);
        if !single {
            z.add_dir(&prefix, unix_secs(SystemTime::now()))?;
        }
        for (rel, meta) in files {
            let Ok(mut f) = std::fs::File::open(paths::join(&dir, &rel)) else { continue };
            let name = if single { rel.clone() } else { format!("{prefix}/{rel}") };
            let mtime = meta.modified().map(unix_secs).unwrap_or(0);
            z.add_file(&name, mtime, meta.len(), &mut f)?;
        }
        z.finish()?;
        Ok(())
    });
    let zip_name = format!("{base_name}.zip");
    Response::new(200)
        .header("Content-Type", "application/zip")
        .header("Content-Disposition", &disposition("attachment", &zip_name))
        .with_body(Body::Stream(stream))
}

/// `If-Match: <sha256>` or `If-Match: none`: refuse to replace or delete a
/// file that changed since the client last saw it (sync uses this).
fn precondition(req: &Request, path: &Path) -> Option<Response> {
    let want = req.header("if-match")?;
    let current = match std::fs::symlink_metadata(path) {
        Ok(m) if m.is_file() => sha256::file_hex(path).ok(),
        Ok(_) => Some("dir".into()),
        Err(_) => None,
    };
    let ok = match (want, &current) {
        ("none", None) => true,
        (w, Some(c)) => w == c,
        _ => false,
    };
    if ok {
        None
    } else {
        Some(Response::error(412, "the file changed on this side; not overwriting it"))
    }
}

fn put_file(shared: &Shared, req: &Request, rel: &str, body: &mut dyn Read) -> Response {
    if let Some(r) = deny_write(shared) {
        return r;
    }
    if req.content_length > shared.cfg.max_upload {
        return Response::error(413, &format!("upload is larger than the {} limit", util::human_size(shared.cfg.max_upload)));
    }
    let rel = match paths::clean_rel(rel) {
        Ok(r) if !r.is_empty() => r,
        Ok(_) => return Response::error(400, "no file name"),
        Err(e) => return perr(e),
    };
    let target = match paths::resolve_for_write(&shared.root, &rel) {
        Ok(t) => t,
        Err(e) => return perr(e),
    };
    if target.is_dir() {
        return Response::error(409, "a folder with that name exists");
    }
    if let Some(r) = precondition(req, &target) {
        return r;
    }
    let dir = target.parent().unwrap_or(&shared.root).to_path_buf();
    let tmp = dir.join(format!("{}tmp-{}", crate::ignore::TEMP_PREFIX, sha256::hex(&rand::bytes::<6>().unwrap_or([7; 6]))));
    let result = (|| -> io::Result<String> {
        let mut f = std::fs::File::create(&tmp)?;
        let mut h = Sha256::new();
        let mut buf = vec![0u8; 256 * 1024];
        let mut left = req.content_length;
        while left > 0 {
            let want = left.min(buf.len() as u64) as usize;
            let n = body.read(&mut buf[..want])?;
            if n == 0 {
                return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "upload was cut short"));
            }
            h.update(&buf[..n]);
            f.write_all(&buf[..n])?;
            left -= n as u64;
        }
        if let Some(t) = req.header("x-mtime").and_then(|v| v.parse::<u64>().ok()) {
            let _ = f.set_modified(SystemTime::UNIX_EPOCH + Duration::from_secs(t));
        }
        f.flush()?;
        drop(f);
        std::fs::rename(&tmp, &target)?;
        Ok(h.finish_hex())
    })();
    match result {
        Ok(hash) => {
            shared.bytes_in.fetch_add(req.content_length, Ordering::Relaxed);
            shared.bump_sync(&[&rel], req.header("x-sync").is_some());
            if req.header("x-sync").is_none() {
                shared.log(&format!("{} uploaded {rel} ({})", req.peer.ip(), util::human_size(req.content_length)));
            }
            Response::json(201, &Json::obj().with("path", rel).with("size", req.content_length).with("hash", hash))
        }
        Err(e) => {
            let _ = std::fs::remove_file(&tmp);
            ioerr(e)
        }
    }
}

fn delete(shared: &Shared, req: &Request, rel: &str) -> Response {
    if let Some(r) = deny_write(shared) {
        return r;
    }
    let rel = match paths::clean_rel(rel) {
        Ok(r) if !r.is_empty() => r,
        Ok(_) => return Response::error(400, "can't delete the shared folder itself"),
        Err(e) => return perr(e),
    };
    let (parent_rel, name) = rel.rsplit_once('/').unwrap_or(("", &rel));
    let parent = match paths::resolve_existing(&shared.root, parent_rel) {
        Ok(p) => p,
        Err(e) => return perr(e),
    };
    let target = parent.join(name);
    let meta = match std::fs::symlink_metadata(&target) {
        Ok(m) => m,
        Err(e) => {
            if req.header("if-match") == Some("none") {
                return Response::json(200, &Json::obj().with("path", rel.as_str()));
            }
            return ioerr(e);
        }
    };
    if let Some(r) = precondition(req, &target) {
        return r;
    }
    let r = if meta.is_dir() { std::fs::remove_dir_all(&target) } else { std::fs::remove_file(&target) };
    if let Err(e) = r {
        return ioerr(e);
    }
    let sync = req.header("x-sync").is_some();
    if sync {
        // a sync delete also removes folders it left empty, as git does
        let mut dir = parent.clone();
        while dir != shared.root && dir.starts_with(&shared.root) && std::fs::remove_dir(&dir).is_ok() {
            match dir.parent() {
                Some(p) => dir = p.to_path_buf(),
                None => break,
            }
        }
    } else {
        shared.log(&format!("{} deleted {rel}", req.peer.ip()));
    }
    shared.bump_sync(&[&rel], false);
    Response::json(200, &Json::obj().with("path", rel.as_str()))
}

fn move_entry(shared: &Shared, from: &str, to: &str) -> Response {
    let from = match paths::clean_rel(from) {
        Ok(r) if !r.is_empty() => r,
        Ok(_) => return Response::error(400, "can't move the shared folder"),
        Err(e) => return perr(e),
    };
    let src = match paths::resolve_existing(&shared.root, &from) {
        Ok(p) => p,
        Err(e) => return perr(e),
    };
    let dst = match paths::resolve_for_write(&shared.root, to) {
        Ok(p) => p,
        Err(e) => return perr(e),
    };
    if std::fs::symlink_metadata(&dst).is_ok() {
        return Response::error(409, "something with that name already exists");
    }
    if dst.starts_with(&src) {
        return Response::error(400, "can't move a folder into itself");
    }
    if let Err(e) = std::fs::rename(&src, &dst) {
        return ioerr(e);
    }
    let to = paths::clean_rel(to).unwrap_or_default();
    shared.bump_sync(&[&from, &to], false);
    shared.log(&format!("moved {from} -> {to}"));
    Response::json(200, &Json::obj().with("from", from).with("to", to))
}

fn pair(shared: &Shared, req: &Request, body: &mut dyn Read) -> Response {
    if req.method != "POST" {
        return Response::error(405, "POST the code");
    }
    if !shared.pairing_open.load(Ordering::Relaxed) {
        return Response::error(403, "pairing by code is off for this share; use the full link");
    }
    let mut buf = Vec::new();
    if body.take(512).read_to_end(&mut buf).is_err() {
        return Response::error(400, "bad body");
    }
    let text = String::from_utf8_lossy(&buf);
    let given = Json::parse(&text)
        .ok()
        .and_then(|j| j.get("code").and_then(|c| c.as_str()).map(str::to_string))
        .or_else(|| util::parse_query(&text).into_iter().find(|(k, _)| k == "code").map(|(_, v)| v))
        .unwrap_or_else(|| text.trim().to_string());
    let given = rand::normalize_code(&given);
    if rand::constant_time_eq(given.as_bytes(), shared.code.as_bytes()) {
        shared.log(&format!("{} paired with the code", req.peer.ip()));
        return Response::json(200, &Json::obj().with("token", shared.token.as_str()).with("info", info(shared)))
            .header("Set-Cookie", &cookie_header(shared));
    }
    let ip = req.peer.ip();
    if shared.strike(ip, true) {
        shared.log(&format!("blocked {ip} for 5 minutes after repeated wrong codes"));
    }
    let total = shared.pair_failures.fetch_add(1, Ordering::Relaxed) + 1;
    if total >= PAIR_GLOBAL_LIMIT && shared.pairing_open.swap(false, Ordering::Relaxed) {
        shared.log("too many wrong pairing codes: pairing is now off for this share (the link still works)");
    }
    Response::error(403, "wrong code")
}

// ---------------------------------------------------------------- sync host

fn start_sync_host(shared: &Arc<Shared>) -> io::Result<()> {
    let mut guard = shared.sync.lock().unwrap();
    if guard.is_some() {
        return Ok(());
    }
    if shared.single.is_some() {
        return Err(io::Error::other("a single-file share can't be synced"));
    }
    let mut index = Index::new(&shared.root, shared.cfg.include_heavy);
    index.scan()?;
    *guard = Some(SyncHost { index, version: 1, last_poll: Instant::now() });
    drop(guard);
    let s = shared.clone();
    std::thread::spawn(move || {
        while !s.stopping() {
            std::thread::sleep(SCAN_EVERY);
            let mut guard = s.sync.lock().unwrap();
            let Some(host) = guard.as_mut() else { break };
            if host.last_poll.elapsed() > SYNC_IDLE {
                continue;
            }
            if let Ok(true) = host.index.scan() {
                host.version += 1;
                s.sync_cv.notify_all();
            }
        }
    });
    shared.log("sync peer connected; watching the folder for changes");
    Ok(())
}

fn manifest(shared: &Arc<Shared>, req: &Request) -> Response {
    if let Err(e) = start_sync_host(shared) {
        return ioerr(e);
    }
    let since: u64 = req.query("since").and_then(|s| s.parse().ok()).unwrap_or(0);
    let wait = Duration::from_millis(req.query("wait").and_then(|s| s.parse().ok()).unwrap_or(0u64).min(25_000));
    let deadline = Instant::now() + wait;
    let mut guard = shared.sync.lock().unwrap();
    loop {
        let host = guard.as_mut().expect("sync host started");
        host.last_poll = Instant::now();
        if host.version != since || shared.stopping() {
            break;
        }
        let now = Instant::now();
        if now >= deadline {
            break;
        }
        guard = shared.sync_cv.wait_timeout(guard, deadline - now).unwrap().0;
    }
    let host = guard.as_ref().expect("sync host started");
    let mut out = Json::obj()
        .with("version", host.version)
        .with("host", shared.host.as_str())
        .with("include_heavy", shared.cfg.include_heavy)
        .with("writable", shared.cfg.writable);
    if host.version == since {
        out.set("same", true);
    } else {
        let files: Vec<Json> = host
            .index
            .files
            .iter()
            .map(|(p, e)| Json::Arr(vec![Json::from(p.as_str()), Json::from(e.hash.as_str()), Json::from(e.size)]))
            .collect();
        out.set("files", files);
    }
    Response::json(200, &out)
}

pub mod ui {
    pub const PAGE: &str = include_str!("ui.html");
    pub const LOGIN: &str = include_str!("login.html");
}
