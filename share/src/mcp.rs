//! `wetwaresuite-share mcp`: MCP over stdio (newline-delimited JSON-RPC 2.0).
//! Shares and syncs run on background threads inside this process and stop
//! when Claude calls the matching stop tool or the session ends.

use std::io::{self, BufRead, Write};
use std::net::{IpAddr, Ipv4Addr};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use crate::client;
use crate::discovery;
use crate::internet::{self, Exposure};
use crate::json::Json;
use crate::server::{Config, Share};
use crate::sync;
use crate::util;

const PROTOCOL_VERSIONS: [&str; 4] = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"];

const INSTRUCTIONS: &str = "wetwaresuite-share shares folders over the LAN or internet and live-syncs folders between \
machines. share_start serves a folder to browsers (read-only unless write=true) and returns a link with an access \
token plus a 6-character pairing code; give the person the link or code. sync_start without a peer hosts a folder \
for live two-way sync; sync_start with peer (a code or link from the other machine) joins one. send_file serves one \
file or folder for download, or uploads it into a writable share when `to` is given. list_remote and fetch_remote \
read another share by link or code. Everything runs in this process until the matching *_stop tool is called. Links \
carry a secret token: only give them to the people they're for. LAN traffic is plain HTTP.";

struct RunningShare {
    id: String,
    share: Share,
    exposure: Option<Exposure>,
}

struct RunningSync {
    id: String,
    handle: sync::SyncHandle,
}

#[derive(Default)]
pub struct State {
    shares: Vec<RunningShare>,
    syncs: Vec<RunningSync>,
    next: u32,
}

fn prop(kind: &str, desc: &str) -> Json {
    Json::obj().with("type", kind).with("description", desc)
}

fn tool(name: &str, title: &str, desc: &str, props: Vec<(&str, Json)>, required: &[&str], read_only: bool) -> Json {
    let properties = Json::Obj(props.into_iter().map(|(k, v)| (k.to_string(), v)).collect());
    Json::obj()
        .with("name", name)
        .with("title", title)
        .with("description", desc)
        .with(
            "inputSchema",
            Json::obj()
                .with("type", "object")
                .with("properties", properties)
                .with("required", required.iter().map(|r| Json::from(*r)).collect::<Vec<_>>()),
        )
        .with(
            "annotations",
            Json::obj()
                .with("title", title)
                .with("readOnlyHint", read_only)
                .with("destructiveHint", false)
                .with("openWorldHint", true),
        )
}

pub fn tools() -> Vec<Json> {
    let id = || prop("string", "id returned by the start tool; omit to act on all");
    vec![
        tool(
            "share_start",
            "Share a folder",
            "Serve a folder (or one file) to browsers and other machines. Returns the link with token, the LAN \
             links, the 6-character pairing code, and the internet link when internet=true.",
            vec![
                ("folder", prop("string", "folder or file to share (absolute path recommended); default: current directory")),
                ("write", prop("boolean", "allow uploads, new folders, renames and deletes (default false)")),
                ("internet", prop("boolean", "also expose it beyond the LAN via cloudflared or tailscale if installed (default false)")),
                ("port", prop("integer", "TCP port (default 8740, or the next free one)")),
                ("local", prop("boolean", "listen on 127.0.0.1 only (default false: all interfaces)")),
            ],
            &[],
            false,
        ),
        tool("share_stop", "Stop sharing", "Stop a share started with share_start or send_file.", vec![("id", id())], &[], false),
        tool("share_status", "Share status", "List running shares and syncs with links, codes and traffic counters.", vec![], &[], true),
        tool(
            "share_peers",
            "Find shares nearby",
            "Listen for wetwaresuite shares announced on the local network.",
            vec![("seconds", prop("number", "how long to listen (default 3)"))],
            &[],
            true,
        ),
        tool(
            "sync_start",
            "Start live sync",
            "Live two-way sync of a folder. Without peer: host the folder and return a code/link for the other \
             machine. With peer (a 6-character code or http:// link from the other machine): join it. Ignores \
             .gitignore/.shareignore matches and .git, node_modules, target, build, .venv, __pycache__ unless \
             include_heavy. Conflicting edits keep both copies (name.conflict-<host>.ext).",
            vec![
                ("folder", prop("string", "local folder to sync (created if missing)")),
                ("peer", prop("string", "code or link of the other machine's sync host; omit to host")),
                ("include_heavy", prop("boolean", "also sync .git, node_modules, target, build, .venv, __pycache__ (default false)")),
            ],
            &["folder"],
            false,
        ),
        tool("sync_status", "Sync status", "Show each sync's state, counters and recent changes.", vec![("id", id())], &[], true),
        tool("sync_stop", "Stop sync", "Stop a sync (hosted or joined).", vec![("id", id())], &[], false),
        tool(
            "send_file",
            "Send a file or folder",
            "Without `to`: serve the file or folder read-only for download and return its link and code. With \
             `to` (code or link of a writable share, e.g. one started by `wetwaresuite-share receive`): upload it there.",
            vec![
                ("path", prop("string", "file or folder to send")),
                ("to", prop("string", "code or link of a writable share to upload into")),
            ],
            &["path"],
            false,
        ),
        tool(
            "list_remote",
            "List a remote share",
            "List one folder of another share.",
            vec![
                ("url", prop("string", "share link (http://...?k=TOKEN) or 6-character code")),
                ("path", prop("string", "folder inside the share (default: top)")),
            ],
            &["url"],
            true,
        ),
        tool(
            "fetch_remote",
            "Download from a share",
            "Download a file or folder from another share into a local folder (keeps its name; overwrites).",
            vec![
                ("url", prop("string", "share link (http://...?k=TOKEN) or 6-character code")),
                ("path", prop("string", "file or folder inside the share (default: everything)")),
                ("dest", prop("string", "local folder to save into (default: current directory)")),
            ],
            &["url"],
            false,
        ),
    ]
}

fn text_result(text: &str, structured: Option<Json>, is_error: bool) -> Json {
    let mut r = Json::obj()
        .with("content", vec![Json::obj().with("type", "text").with("text", text)])
        .with("isError", is_error);
    if let Some(s) = structured {
        r.set("structuredContent", s);
    }
    r
}

fn err_result(msg: impl std::fmt::Display) -> Json {
    text_result(&msg.to_string(), None, true)
}

fn stderr_logger() -> crate::server::Logger {
    Arc::new(|m: &str| eprintln!("wetwaresuite-share: {m}"))
}

fn share_json(r: &RunningShare) -> Json {
    let s = &r.share;
    Json::obj()
        .with("id", r.id.as_str())
        .with("kind", s.shared.cfg.kind.as_str())
        .with("folder", s.shared.shared_path().to_string_lossy().as_ref())
        .with("writable", s.shared.cfg.writable && s.shared.single.is_none())
        .with("url", s.urls().first().cloned().unwrap_or_else(|| s.local_url()))
        .with("lan_urls", s.urls().into_iter().map(Json::from).collect::<Vec<_>>())
        .with("local_url", s.local_url())
        .with("internet_url", r.exposure.as_ref().and_then(|e| e.url().map(str::to_string)))
        .with("code", s.code())
        .with("token", s.token())
        .with("port", s.port())
        .with("stats", s.shared.status())
}

fn describe_share(r: &RunningShare) -> String {
    let s = &r.share;
    let mut t = format!(
        "{} {} ({})\n  link: {}\n  code: {}\n",
        r.id,
        s.shared.shared_path().display(),
        if s.shared.cfg.writable { "read-write" } else { "read-only" },
        s.urls().first().cloned().unwrap_or_else(|| s.local_url()),
        s.code()
    );
    if let Some(e) = &r.exposure {
        match e.url() {
            Some(u) => t.push_str(&format!("  internet: {u} via {}\n", e.describe())),
            None => t.push_str(&format!("  internet: {}\n", e.describe())),
        }
    }
    t
}

impl State {
    fn new_id(&mut self, prefix: &str) -> String {
        self.next += 1;
        format!("{prefix}{}", self.next)
    }

    fn start_share(&mut self, mut cfg: Config, internet: bool) -> Result<&RunningShare, String> {
        cfg.log = Some(stderr_logger());
        let share = Share::start(cfg).map_err(|e| format!("couldn't start the share: {e}"))?;
        let exposure = internet.then(|| internet::expose(share.port(), share.token(), share.addr.ip().is_loopback()));
        let id = self.new_id("share");
        self.shares.push(RunningShare { id, share, exposure });
        Ok(self.shares.last().expect("just pushed"))
    }

    fn call(&mut self, name: &str, args: &Json) -> Json {
        let s = |k: &str| args.get(k).and_then(Json::as_str).map(str::to_string);
        let b = |k: &str| args.bool_or(k, false);
        match name {
            "share_start" => {
                let folder = s("folder").map(PathBuf::from).unwrap_or_else(|| std::env::current_dir().unwrap_or_default());
                let mut cfg = Config::new(folder);
                cfg.writable = b("write");
                if let Some(p) = args.get("port").and_then(Json::as_u64) {
                    cfg.port = p.min(65535) as u16;
                }
                if b("local") {
                    cfg.bind = IpAddr::V4(Ipv4Addr::LOCALHOST);
                }
                match self.start_share(cfg, b("internet")) {
                    Ok(r) => text_result(&format!("sharing\n{}", describe_share(r)), Some(share_json(r)), false),
                    Err(e) => err_result(e),
                }
            }
            "share_stop" | "sync_stop" => {
                let id = s("id");
                let mut stopped = Vec::new();
                let want = |x: &str| id.as_deref().is_none_or(|i| i == x);
                let only_sync = name == "sync_stop";
                let mut i = 0;
                while i < self.shares.len() {
                    let is_sync_host = self.shares[i].share.shared.cfg.kind == "sync";
                    if want(&self.shares[i].id) && (!only_sync || is_sync_host || id.is_some()) {
                        let mut r = self.shares.remove(i);
                        r.share.stop();
                        if let Some(e) = r.exposure.as_mut() {
                            e.stop();
                        }
                        stopped.push(r.id);
                    } else {
                        i += 1;
                    }
                }
                if only_sync || id.is_some() {
                    let mut i = 0;
                    while i < self.syncs.len() {
                        if want(&self.syncs[i].id) {
                            let mut r = self.syncs.remove(i);
                            r.handle.stop();
                            stopped.push(r.id);
                        } else {
                            i += 1;
                        }
                    }
                }
                let out = Json::obj().with("stopped", stopped.iter().map(Json::from).collect::<Vec<_>>());
                if stopped.is_empty() {
                    text_result("nothing to stop", Some(out), false)
                } else {
                    text_result(&format!("stopped {}", stopped.join(", ")), Some(out), false)
                }
            }
            "share_status" => {
                let mut text = String::new();
                for r in &self.shares {
                    text.push_str(&describe_share(r));
                }
                for r in &self.syncs {
                    let st = r.handle.stats.lock().unwrap();
                    text.push_str(&format!(
                        "{} sync {} <-> {} ({})\n",
                        r.id,
                        r.handle.folder.display(),
                        r.handle.peer,
                        if st.in_sync { "in sync" } else { "syncing" }
                    ));
                }
                let out = Json::obj()
                    .with("shares", self.shares.iter().map(share_json).collect::<Vec<_>>())
                    .with("syncs", self.syncs.iter().map(sync_json).collect::<Vec<_>>());
                text_result(if text.is_empty() { "nothing running" } else { &text }, Some(out), false)
            }
            "share_peers" => {
                let secs = args.get("seconds").and_then(Json::as_f64).unwrap_or(3.0).clamp(0.5, 30.0);
                match discovery::listen(Duration::from_secs_f64(secs), None) {
                    Ok(peers) => {
                        let mut text = String::new();
                        let list: Vec<Json> = peers
                            .iter()
                            .map(|p| {
                                text.push_str(&format!(
                                    "{} on {} ({}, {}{})\n",
                                    p.ann.folder,
                                    p.ann.host,
                                    p.base_url(),
                                    p.ann.kind,
                                    if p.ann.writable { ", writable" } else { "" }
                                ));
                                Json::obj()
                                    .with("host", p.ann.host.as_str())
                                    .with("folder", p.ann.folder.as_str())
                                    .with("kind", p.ann.kind.as_str())
                                    .with("writable", p.ann.writable)
                                    .with("address", p.base_url())
                            })
                            .collect();
                        if text.is_empty() {
                            text = "no shares found on this network (they announce every 2 s over UDP port 8741)".into();
                        }
                        text_result(&text, Some(Json::obj().with("peers", list)), false)
                    }
                    Err(e) => err_result(e),
                }
            }
            "sync_start" => {
                let Some(folder) = s("folder") else { return err_result("folder is required") };
                match s("peer") {
                    None => {
                        if let Err(e) = std::fs::create_dir_all(&folder) {
                            return err_result(format!("{folder}: {e}"));
                        }
                        let mut cfg = Config::new(&folder);
                        cfg.writable = true;
                        cfg.kind = "sync".into();
                        cfg.include_heavy = b("include_heavy");
                        match self.start_share(cfg, false) {
                            Ok(r) => text_result(
                                &format!(
                                    "hosting {folder} for sync. On the other machine run:\n  wetwaresuite-share sync <folder> {}\nor with the link:\n  wetwaresuite-share sync <folder> \"{}\"\n",
                                    r.share.code(),
                                    r.share.urls().first().cloned().unwrap_or_else(|| r.share.local_url())
                                ),
                                Some(share_json(r)),
                                false,
                            ),
                            Err(e) => err_result(e),
                        }
                    }
                    Some(peer) => {
                        let target = match client::resolve(&peer, None) {
                            Ok((t, _)) => t,
                            Err(e) => return err_result(format!("can't reach {peer}: {e}")),
                        };
                        let mut opts = sync::Options::new(&folder, target);
                        opts.include_heavy = b("include_heavy");
                        opts.log = Some(stderr_logger());
                        match sync::start(opts) {
                            Ok(handle) => {
                                let id = self.new_id("sync");
                                // give the first round a moment so the answer says something useful
                                for _ in 0..50 {
                                    if handle.stats.lock().unwrap().rounds > 1 {
                                        break;
                                    }
                                    std::thread::sleep(Duration::from_millis(100));
                                }
                                let r = RunningSync { id, handle };
                                let out = sync_json(&r);
                                let text = format!("syncing {folder} with {} ({})", r.handle.peer, r.id);
                                self.syncs.push(r);
                                text_result(&text, Some(out), false)
                            }
                            Err(e) => err_result(e),
                        }
                    }
                }
            }
            "sync_status" => {
                let id = s("id");
                let list: Vec<Json> =
                    self.syncs.iter().filter(|r| id.as_deref().is_none_or(|i| i == r.id)).map(sync_json).collect();
                let hosts: Vec<Json> = self
                    .shares
                    .iter()
                    .filter(|r| r.share.shared.cfg.kind == "sync" && id.as_deref().is_none_or(|i| i == r.id))
                    .map(share_json)
                    .collect();
                let text = if list.is_empty() && hosts.is_empty() {
                    "no syncs running".to_string()
                } else {
                    let mut t = String::new();
                    for j in &list {
                        let st = j.get("stats").cloned().unwrap_or(Json::Null);
                        t.push_str(&format!(
                            "{}: {} <-> {}: {}, {} files, {} up, {} down, {} conflicts{}\n",
                            j.str_or("id", ""),
                            j.str_or("folder", ""),
                            j.str_or("peer", ""),
                            if st.bool_or("in_sync", false) { "in sync" } else { "syncing" },
                            st.get("files").and_then(Json::as_u64).unwrap_or(0),
                            st.get("uploaded").and_then(Json::as_u64).unwrap_or(0),
                            st.get("downloaded").and_then(Json::as_u64).unwrap_or(0),
                            st.get("conflicts").and_then(Json::as_u64).unwrap_or(0),
                            st.get("last_error").and_then(Json::as_str).map(|e| format!(", error: {e}")).unwrap_or_default()
                        ));
                    }
                    for h in &hosts {
                        t.push_str(&format!("{}: hosting {} (code {})\n", h.str_or("id", ""), h.str_or("folder", ""), h.str_or("code", "")));
                    }
                    t
                };
                text_result(&text, Some(Json::obj().with("syncs", list).with("hosts", hosts)), false)
            }
            "send_file" => {
                let Some(path) = s("path") else { return err_result("path is required") };
                match s("to") {
                    Some(to) => {
                        let target = match client::resolve(&to, None) {
                            Ok((t, _)) => t,
                            Err(e) => return err_result(format!("can't reach {to}: {e}")),
                        };
                        match client::send_to(&target, std::path::Path::new(&path), &|m| eprintln!("wetwaresuite-share: {m}")) {
                            Ok(t) => text_result(
                                &format!("sent {} file(s), {}", t.files, util::human_size(t.bytes)),
                                Some(Json::obj().with("files", t.files).with("bytes", t.bytes)),
                                false,
                            ),
                            Err(e) => err_result(e),
                        }
                    }
                    None => {
                        let mut cfg = Config::new(&path);
                        cfg.kind = "send".into();
                        match self.start_share(cfg, false) {
                            Ok(r) => text_result(
                                &format!(
                                    "serving {path} for download\n{}On the other machine: open the link, or run `wetwaresuite-share receive {}`\n",
                                    describe_share(r),
                                    r.share.code()
                                ),
                                Some(share_json(r)),
                                false,
                            ),
                            Err(e) => err_result(e),
                        }
                    }
                }
            }
            "list_remote" => {
                let Some(url) = s("url") else { return err_result("url is required") };
                let path = s("path").unwrap_or_default();
                let target = match client::resolve(&url, None) {
                    Ok((t, _)) => t,
                    Err(e) => return err_result(format!("can't reach {url}: {e}")),
                };
                match client::list_remote(&target, &path) {
                    Ok(j) => {
                        let mut text = String::new();
                        for e in j.get("entries").and_then(Json::as_array).unwrap_or(&[]) {
                            if e.bool_or("dir", false) {
                                text.push_str(&format!("{}/\n", e.str_or("name", "")));
                            } else {
                                text.push_str(&format!(
                                    "{}  {}\n",
                                    e.str_or("name", ""),
                                    util::human_size(e.get("size").and_then(Json::as_u64).unwrap_or(0))
                                ));
                            }
                        }
                        text_result(if text.is_empty() { "(empty)" } else { &text }, Some(j), false)
                    }
                    Err(e) => err_result(e),
                }
            }
            "fetch_remote" => {
                let Some(url) = s("url") else { return err_result("url is required") };
                let path = s("path").unwrap_or_default();
                let dest = s("dest").map(PathBuf::from).unwrap_or_else(|| std::env::current_dir().unwrap_or_default());
                let target = match client::resolve(&url, None) {
                    Ok((t, _)) => t,
                    Err(e) => return err_result(format!("can't reach {url}: {e}")),
                };
                match client::fetch(&target, &path, &dest, &|m| eprintln!("wetwaresuite-share: {m}")) {
                    Ok(t) => text_result(
                        &format!("downloaded {} file(s), {} into {}", t.files, util::human_size(t.bytes), t.saved_to.display()),
                        Some(
                            Json::obj()
                                .with("files", t.files)
                                .with("bytes", t.bytes)
                                .with("saved_to", t.saved_to.to_string_lossy().as_ref()),
                        ),
                        false,
                    ),
                    Err(e) => err_result(e),
                }
            }
            _ => err_result(format!("unknown tool: {name}")),
        }
    }

    pub fn shutdown(&mut self) {
        for r in &mut self.syncs {
            r.handle.stop();
        }
        for r in &mut self.shares {
            r.share.stop();
            if let Some(e) = r.exposure.as_mut() {
                e.stop();
            }
        }
        self.syncs.clear();
        self.shares.clear();
    }
}

fn sync_json(r: &RunningSync) -> Json {
    Json::obj()
        .with("id", r.id.as_str())
        .with("folder", r.handle.folder.to_string_lossy().as_ref())
        .with("peer", r.handle.peer.as_str())
        .with("running", r.handle.running())
        .with("stats", r.handle.stats.lock().unwrap().to_json())
}

fn rpc_result(id: &Json, result: Json) -> Json {
    Json::obj().with("jsonrpc", "2.0").with("id", id.clone()).with("result", result)
}

fn rpc_error(id: &Json, code: i64, message: &str) -> Json {
    Json::obj()
        .with("jsonrpc", "2.0")
        .with("id", id.clone())
        .with("error", Json::obj().with("code", code).with("message", message))
}

/// Handles one JSON-RPC line; `None` for notifications.
pub fn handle_line(state: &mut State, line: &str) -> Option<String> {
    let msg = match Json::parse(line) {
        Ok(m @ Json::Obj(_)) => m,
        _ => return Some(rpc_error(&Json::Null, -32700, "parse error").dump()),
    };
    let id = msg.get("id").cloned();
    let Some(method) = msg.get("method").and_then(Json::as_str) else {
        return id.map(|id| rpc_error(&id, -32600, "invalid request").dump());
    };
    let id = id?; // notifications (notifications/initialized, cancelled...) need no answer
    let params = msg.get("params").cloned().unwrap_or_else(Json::obj);
    let result = match method {
        "initialize" => {
            let requested = params.str_or("protocolVersion", "");
            let version = PROTOCOL_VERSIONS.iter().find(|v| **v == requested).unwrap_or(&PROTOCOL_VERSIONS[0]);
            Json::obj()
                .with("protocolVersion", *version)
                .with("capabilities", Json::obj().with("tools", Json::obj().with("listChanged", false)))
                .with("serverInfo", Json::obj().with("name", "wetwaresuite-share").with("version", crate::VERSION))
                .with("instructions", INSTRUCTIONS)
        }
        "ping" => Json::obj(),
        "tools/list" => Json::obj().with("tools", tools()),
        "tools/call" => {
            let name = params.str_or("name", "").to_string();
            let args = params.get("arguments").cloned().unwrap_or_else(Json::obj);
            if !tools().iter().any(|t| t.str_or("name", "") == name) {
                return Some(rpc_error(&id, -32602, &format!("unknown tool: {name}")).dump());
            }
            if !matches!(args, Json::Obj(_) | Json::Null) {
                return Some(rpc_result(&id, err_result("arguments must be an object")).dump());
            }
            state.call(&name, &args)
        }
        "resources/list" => Json::obj().with("resources", Vec::<Json>::new()),
        "resources/templates/list" => Json::obj().with("resourceTemplates", Vec::<Json>::new()),
        "prompts/list" => Json::obj().with("prompts", Vec::<Json>::new()),
        _ => return Some(rpc_error(&id, -32601, &format!("method not found: {method}")).dump()),
    };
    Some(rpc_result(&id, result).dump())
}

pub fn run() -> io::Result<()> {
    let mut state = State::default();
    let stdin = io::stdin();
    let mut out = io::stdout().lock();
    for line in stdin.lock().lines() {
        let line = line?;
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if let Some(resp) = handle_line(&mut state, line) {
            out.write_all(resp.as_bytes())?;
            out.write_all(b"\n")?;
            out.flush()?;
        }
    }
    state.shutdown();
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn initialize_and_list() {
        let mut st = State::default();
        let r = handle_line(&mut st, r#"{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}"#)
            .unwrap();
        let j = Json::parse(&r).unwrap();
        let res = j.get("result").unwrap();
        assert_eq!(res.str_or("protocolVersion", ""), "2025-06-18");
        assert!(res.str_or("instructions", "").contains("sync_start"));
        let r = handle_line(&mut st, r#"{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1999-01-01"}}"#)
            .unwrap();
        assert!(r.contains("2025-11-25"));
        assert!(handle_line(&mut st, r#"{"jsonrpc":"2.0","method":"notifications/initialized"}"#).is_none());
        let r = handle_line(&mut st, r#"{"jsonrpc":"2.0","id":"x","method":"tools/list"}"#).unwrap();
        let j = Json::parse(&r).unwrap();
        let tools = j.get("result").unwrap().get("tools").unwrap().as_array().unwrap().to_vec();
        let names: Vec<&str> = tools.iter().map(|t| t.str_or("name", "")).collect();
        for want in [
            "share_start", "share_stop", "share_status", "share_peers", "sync_start", "sync_status", "sync_stop",
            "send_file", "list_remote", "fetch_remote",
        ] {
            assert!(names.contains(&want), "{want}");
        }
        for t in &tools {
            assert_eq!(t.get("inputSchema").unwrap().str_or("type", ""), "object");
        }
        assert_eq!(j.get("id").unwrap().as_str(), Some("x"));
    }

    #[test]
    fn errors() {
        let mut st = State::default();
        assert!(handle_line(&mut st, "not json").unwrap().contains("-32700"));
        assert!(handle_line(&mut st, r#"{"jsonrpc":"2.0","id":2,"method":"nope"}"#).unwrap().contains("-32601"));
        let r = handle_line(&mut st, r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"bogus"}}"#).unwrap();
        assert!(r.contains("-32602"));
        let r = handle_line(&mut st, r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"sync_start","arguments":{}}}"#)
            .unwrap();
        assert!(r.contains("\"isError\":true"));
    }

    #[test]
    fn share_start_status_stop() {
        let dir = std::env::temp_dir().join(format!("wetwaresuite-share-mcp-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("a.txt"), "hi").unwrap();
        let mut st = State::default();
        let call = format!(
            r#"{{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{{"name":"share_start","arguments":{{"folder":{},"local":true,"port":0}}}}}}"#,
            Json::from(dir.to_string_lossy().as_ref()).dump()
        );
        let r = Json::parse(&handle_line(&mut st, &call).unwrap()).unwrap();
        let res = r.get("result").unwrap();
        assert_eq!(res.get("isError"), Some(&Json::Bool(false)));
        let sc = res.get("structuredContent").unwrap();
        assert_eq!(sc.str_or("code", "").len(), 6);
        assert!(sc.str_or("local_url", "").starts_with("http://127.0.0.1:"));
        // the link works
        let target = crate::http::Target::parse(sc.str_or("local_url", "")).unwrap();
        let listing = client::list_remote(&target, "").unwrap();
        assert_eq!(listing.get("entries").unwrap().as_array().unwrap().len(), 1);
        let r = handle_line(&mut st, r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"share_status","arguments":{}}}"#)
            .unwrap();
        assert!(r.contains("share1"));
        let r = handle_line(&mut st, r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"share_stop","arguments":{}}}"#)
            .unwrap();
        assert!(r.contains("stopped share1"));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
