//! Client side of shares: find a share by code or link, list it, download
//! files or folders, and upload into a writable share.

use std::cell::RefCell;
use std::io;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use crate::discovery::{self, Peer};
use crate::http::{Client, ReqBody, Target};
use crate::json::Json;
use crate::paths;
use crate::rand;
use crate::util;

/// How long `resolve` listens for LAN announcements when given a code.
pub const DISCOVERY_WAIT: Duration = Duration::from_secs(8);

fn try_pair(base: &str, code: &str) -> io::Result<Target> {
    let mut t = Target::parse(base).map_err(io::Error::other)?;
    let mut c = Client::new(t.clone());
    c.timeout = Duration::from_secs(5);
    let body = Json::obj().with("code", code).dump();
    let j = c.fetch_json("POST", "/api/pair", ReqBody::Bytes(body.as_bytes()))?;
    t.token = j.str_or("token", "").to_string();
    if t.token.is_empty() {
        return Err(io::Error::other("pairing returned no token"));
    }
    Ok(t)
}

/// Turns what a person typed (a share link, or a 6-character code, or a
/// link without token plus a code) into a reachable, authenticated target.
pub fn resolve(spec: &str, code: Option<&str>) -> io::Result<(Target, Json)> {
    let spec = spec.trim();
    let target = if rand::looks_like_code(spec) {
        let code = rand::normalize_code(spec);
        let found: RefCell<Option<Target>> = RefCell::new(None);
        let want = |p: &Peer| match try_pair(&p.base_url(), &code) {
            Ok(t) => {
                *found.borrow_mut() = Some(t);
                true
            }
            Err(_) => false,
        };
        let peers = discovery::listen(DISCOVERY_WAIT, Some(&want))?;
        let result = found.borrow_mut().take();
        match result {
            Some(t) => t,
            None if peers.is_empty() => {
                return Err(io::Error::new(
                    io::ErrorKind::NotFound,
                    format!(
                        "no share answered on this network within {}s. Check both machines are on the same Wi-Fi/LAN \
                         and the firewall allows wetwaresuite-share, or use the full http:// link instead of the code",
                        DISCOVERY_WAIT.as_secs()
                    ),
                ))
            }
            None => {
                return Err(io::Error::new(
                    io::ErrorKind::NotFound,
                    format!("found {} share(s) nearby but none accepted code {code}", peers.len()),
                ))
            }
        }
    } else if spec.len() == 6 && spec.chars().all(|c| c.is_ascii_alphanumeric()) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{spec} isn't a pairing code: codes use 2-9 and A-Z without I, L or O (check for 0/O and 1/I mix-ups)"),
        ));
    } else {
        let t = Target::parse(spec).map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;
        if t.token.is_empty() {
            match code {
                Some(c) => try_pair(&t.base_url(), &rand::normalize_code(c))?,
                None => {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "that link has no ?k= token; paste the full link the share printed, or add --code CODE",
                    ))
                }
            }
        } else {
            t
        }
    };
    let mut c = Client::new(target.clone());
    let info = c.fetch_json("GET", "/api/info", ReqBody::None)?;
    Ok((target, info))
}

pub fn list_remote(target: &Target, path: &str) -> io::Result<Json> {
    let mut c = Client::new(target.clone());
    c.fetch_json("GET", &format!("/api/list?path={}", util::percent_encode(path)), ReqBody::None)
}

/// Streams one remote file into `dest` via a temporary file.
pub fn download(c: &mut Client, rel: &str, dest: &Path, mtime: Option<u64>) -> io::Result<u64> {
    let dir = dest.parent().unwrap_or(Path::new("."));
    let tmp = dir.join(format!("{}dl-{}", crate::ignore::TEMP_PREFIX, crate::sha256::hex(&rand::bytes::<6>()?)));
    let mut f = std::fs::File::create(&tmp)?;
    let result = c.call("GET", &format!("/f/{}", util::encode_path(rel)), &[], ReqBody::None, &mut f);
    let resp = match result {
        Ok(r) => r,
        Err(e) => {
            drop(f);
            let _ = std::fs::remove_file(&tmp);
            return Err(e);
        }
    };
    if resp.status != 200 {
        drop(f);
        let body = std::fs::read(&tmp).unwrap_or_default();
        let _ = std::fs::remove_file(&tmp);
        return Err(crate::http::http_error(resp.status, &body));
    }
    if let Some(t) = mtime {
        let _ = f.set_modified(SystemTime::UNIX_EPOCH + Duration::from_secs(t));
    }
    let n = f.metadata()?.len();
    drop(f);
    std::fs::rename(&tmp, dest).inspect_err(|_| {
        let _ = std::fs::remove_file(&tmp);
    })?;
    Ok(n)
}

#[derive(Debug, Default, Clone)]
pub struct Transfer {
    pub files: u64,
    pub bytes: u64,
    pub saved_to: PathBuf,
}

/// Downloads `remote` (a file or folder; "" for the whole share) into the
/// local folder `dest`, keeping its name. Existing files are overwritten.
pub fn fetch(target: &Target, remote: &str, dest: &Path, progress: &dyn Fn(&str)) -> io::Result<Transfer> {
    let remote = paths::clean_rel(remote).map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e.to_string()))?;
    let mut c = Client::new(target.clone());
    let info = c.fetch_json("GET", "/api/info", ReqBody::None)?;
    let tree = c.fetch_json("GET", &format!("/api/tree?path={}", util::percent_encode(&remote)), ReqBody::None)?;
    std::fs::create_dir_all(dest)?;
    let dest_root = paths::canonical_root(dest)?;
    let single = info.bool_or("single", false);
    // the local name for the top of what we fetch
    let top = if remote.is_empty() {
        if single { String::new() } else { info.str_or("folder", "share").to_string() }
    } else {
        remote.rsplit('/').next().unwrap_or(&remote).to_string()
    };
    if !top.is_empty() {
        paths::check_segment(&top).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
    }
    let files = tree.get("files").and_then(Json::as_array).unwrap_or(&[]).to_vec();
    let mut t = Transfer { saved_to: dest_root.join(&top), ..Default::default() };
    for f in files {
        let path = f.str_or("path", "");
        // never trust the server's paths: they must be clean and under what we asked for
        let rel = paths::clean_rel(path).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
        let under = if remote.is_empty() {
            rel.clone()
        } else if rel == remote {
            String::new()
        } else if let Some(rest) = rel.strip_prefix(&format!("{remote}/")) {
            rest.to_string()
        } else {
            continue;
        };
        let local_rel = match (top.is_empty(), under.is_empty()) {
            (true, _) => under.clone(),
            (false, true) => top.clone(),
            (false, false) => format!("{top}/{under}"),
        };
        let local = paths::resolve_for_write(&dest_root, &local_rel)
            .map_err(|e| io::Error::other(format!("{local_rel}: {e}")))?;
        progress(&format!("↓ {local_rel} ({})", util::human_size(f.get("size").and_then(Json::as_u64).unwrap_or(0))));
        t.bytes += download(&mut c, &rel, &local, f.get("mtime").and_then(Json::as_u64))?;
        t.files += 1;
        if top.is_empty() {
            t.saved_to = local; // a single shared file: report the file itself
        }
    }
    let _ = c.fetch("POST", "/api/done", ReqBody::None);
    Ok(t)
}

fn local_files(path: &Path, rel: &str, out: &mut Vec<(PathBuf, String)>) -> io::Result<()> {
    let meta = std::fs::symlink_metadata(path)?;
    if meta.is_file() {
        out.push((path.to_path_buf(), rel.to_string()));
    } else if meta.is_dir() {
        let mut entries: Vec<_> = std::fs::read_dir(path)?.filter_map(Result::ok).collect();
        entries.sort_by_key(|e| e.file_name());
        for e in entries {
            let Some(name) = e.file_name().to_str().map(str::to_string) else { continue };
            if name.starts_with(crate::ignore::TEMP_PREFIX) {
                continue;
            }
            local_files(&e.path(), &format!("{rel}/{name}"), out)?;
        }
    }
    Ok(())
}

/// Uploads a local file or folder into the top of a writable share.
pub fn send_to(target: &Target, local: &Path, progress: &dyn Fn(&str)) -> io::Result<Transfer> {
    let name = local
        .canonicalize()?
        .file_name()
        .and_then(|n| n.to_str())
        .map(str::to_string)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "can't send a path without a name"))?;
    let mut files = Vec::new();
    local_files(local, &name, &mut files)?;
    let mut c = Client::new(target.clone());
    let info = c.fetch_json("GET", "/api/info", ReqBody::None)?;
    if !info.bool_or("writable", false) {
        return Err(io::Error::new(io::ErrorKind::PermissionDenied, "that share is read-only; the receiver needs `receive` or `serve --write`"));
    }
    let mut t = Transfer { saved_to: PathBuf::from(&name), ..Default::default() };
    for (path, rel) in files {
        let meta = std::fs::metadata(&path)?;
        let mtime = meta.modified().map(util::unix_secs).unwrap_or(0).to_string();
        progress(&format!("↑ {rel} ({})", util::human_size(meta.len())));
        let mut out = Vec::new();
        let resp = c.call(
            "PUT",
            &format!("/f/{}", util::encode_path(&rel)),
            &[("X-Mtime", &mtime)],
            ReqBody::File(&path),
            &mut out,
        )?;
        if resp.status >= 300 {
            return Err(crate::http::http_error(resp.status, &out));
        }
        t.files += 1;
        t.bytes += meta.len();
    }
    let _ = c.fetch("POST", "/api/done", ReqBody::None);
    Ok(t)
}
