//! wetwaresuite-share: share a folder with any browser, live-sync folders
//! between machines, send one-off files. See docs/tutorials/share.md.

use std::io::IsTerminal;
use std::net::{IpAddr, Ipv4Addr};
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;
use std::time::Duration;

use share::internet::{self, Exposure};
use share::qr::{self, QrCode};
use share::server::{Config, Logger, Share};
use share::{client, discovery, mcp, rand, sync, util, VERSION};

const USAGE: &str = "\
wetwaresuite-share: share and live-sync folders over LAN or internet

usage:
  wetwaresuite-share                          share the current folder (read-only)
  wetwaresuite-share serve [FOLDER]           share a folder; the other side just needs a browser
  wetwaresuite-share sync FOLDER              host FOLDER for live two-way sync; prints a code
  wetwaresuite-share sync FOLDER CODE|LINK    join a sync host and keep FOLDER in sync with it
  wetwaresuite-share send PATH                offer a file or folder for download; prints a code
  wetwaresuite-share send PATH CODE|LINK      upload a file or folder into a waiting `receive`
  wetwaresuite-share receive CODE|LINK        download what a `send` is offering
  wetwaresuite-share receive                  wait for a `send PATH CODE`; files land in --to DIR
  wetwaresuite-share peers                    list shares announced on this network
  wetwaresuite-share mcp                      MCP server on stdio, for Claude

options:
  --write            allow uploads, renames and deletes from the browser (serve)
  --internet         also reach it from outside the LAN: cloudflared tunnel, else tailscale
  --local            listen on 127.0.0.1 only (default: all interfaces, i.e. 0.0.0.0)
  --bind IP          listen on one address
  --port N           TCP port (default 8740, or the next free one)
  --max-upload MB    largest upload accepted (default 4096)
  --include-heavy    also sync .git, node_modules, target, build, .venv, __pycache__
  --code CODE        pairing code for a link without ?k= (sync/receive/send)
  --to DIR           where `receive` saves (default: current folder)
  --keep             `send`/`receive`: keep running after the first transfer
  --no-qr            don't print the QR code
  --no-announce      don't announce on the LAN (joining by code then needs the link)
  --wait SECONDS     how long `peers` listens (default 3)
  --version
";

#[derive(Default)]
struct Opts {
    pos: Vec<String>,
    write: bool,
    internet: bool,
    local: bool,
    bind: Option<IpAddr>,
    port: Option<u16>,
    max_upload_mb: Option<u64>,
    include_heavy: bool,
    code: Option<String>,
    to: Option<PathBuf>,
    keep: bool,
    no_qr: bool,
    no_announce: bool,
    wait: Option<f64>,
}

fn parse(args: &[String]) -> Result<Opts, String> {
    let mut o = Opts::default();
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = |name: &str| it.next().cloned().ok_or(format!("{name} needs a value"));
        match a.as_str() {
            "--write" | "-w" => o.write = true,
            "--internet" => o.internet = true,
            "--local" => o.local = true,
            "--include-heavy" => o.include_heavy = true,
            "--keep" => o.keep = true,
            "--no-qr" => o.no_qr = true,
            "--no-announce" => o.no_announce = true,
            "--bind" => o.bind = Some(val("--bind")?.parse().map_err(|_| "--bind needs an IP address")?),
            "--port" | "-p" => o.port = Some(val("--port")?.parse().map_err(|_| "--port needs a number 0-65535")?),
            "--max-upload" => o.max_upload_mb = Some(val("--max-upload")?.parse().map_err(|_| "--max-upload needs MB")?),
            "--code" => o.code = Some(val("--code")?),
            "--to" => o.to = Some(PathBuf::from(val("--to")?)),
            "--wait" => o.wait = Some(val("--wait")?.parse().map_err(|_| "--wait needs seconds")?),
            s if s.starts_with('-') && s.len() > 1 => return Err(format!("unknown option {s}")),
            s => o.pos.push(s.to_string()),
        }
    }
    Ok(o)
}

fn logger() -> Logger {
    Arc::new(|m: &str| println!("  {m}"))
}

fn config(o: &Opts, path: PathBuf) -> Config {
    let mut c = Config::new(path);
    c.writable = o.write;
    if o.local {
        c.bind = IpAddr::V4(Ipv4Addr::LOCALHOST);
    }
    if let Some(b) = o.bind {
        c.bind = b;
    }
    if let Some(p) = o.port {
        c.port = p;
        c.port_fallback = false;
    }
    if let Some(mb) = o.max_upload_mb {
        c.max_upload = mb.saturating_mul(1 << 20);
    }
    c.include_heavy = o.include_heavy;
    c.announce = !o.no_announce;
    c.log = Some(logger());
    c
}

fn print_qr(url: &str) {
    match QrCode::encode(url.as_bytes()) {
        Some(q) => {
            qr::enable_ansi();
            print!("{}", q.to_terminal(2));
        }
        None => println!("  (link too long for a terminal QR code)"),
    }
}

/// Prints the links, code and QR for a running share; returns the exposure
/// (which must stay alive) when --internet was asked for.
fn announce(share: &Share, o: &Opts, what: &str, join_hint: &str) -> Option<Exposure> {
    let folder = share.shared.shared_path();
    let mode = if share.shared.cfg.writable && share.shared.single.is_none() { "read-write" } else { "read-only" };
    println!("{what} {} ({mode}). Press Ctrl+C to stop.\n", folder.display());
    let lan = share.urls();
    for u in &lan {
        println!("  On another device:  {u}");
    }
    println!("  On this machine:    {}", share.local_url());
    println!("  Pairing code:       {}   ({join_hint})", share.code());
    let exposure = o.internet.then(|| {
        println!("\n  Opening an internet route...");
        internet::expose(share.port(), share.token(), share.addr.ip().is_loopback())
    });
    if let Some(e) = &exposure {
        match e.url() {
            Some(u) => println!("  Over the internet:  {u}\n                      via {}", e.describe()),
            None => println!("  Internet: {}", e.describe()),
        }
    }
    let qr_url = exposure.as_ref().and_then(|e| e.url().map(str::to_string)).or_else(|| lan.first().cloned());
    if let Some(u) = qr_url {
        if !o.no_qr && std::io::stdout().is_terminal() {
            println!("\n  Scan with a phone camera:\n");
            print_qr(&u);
        }
    }
    if share.addr.ip().is_unspecified() {
        println!("\n  Listening on all network interfaces (0.0.0.0) port {}; use --local for this machine only.", share.port());
    }
    println!("  LAN traffic is plain HTTP: anyone with the link or code can open it. Only share it with people you trust.\n");
    exposure
}

fn folder_arg(o: &Opts, i: usize) -> Result<PathBuf, String> {
    match o.pos.get(i) {
        Some(p) => Ok(PathBuf::from(p)),
        None => std::env::current_dir().map_err(|e| format!("no current folder: {e}")),
    }
}

/// True for the home folder or a drive/filesystem root: too much to share
/// by accident (e.g. a double-click on macOS starts in the home folder).
fn too_broad(folder: &std::path::Path) -> bool {
    let Ok(canon) = folder.canonicalize() else { return false };
    if canon.parent().is_none() {
        return true;
    }
    ["HOME", "USERPROFILE"]
        .iter()
        .filter_map(std::env::var_os)
        .filter_map(|h| PathBuf::from(h).canonicalize().ok())
        .any(|h| h == canon)
}

fn serve(o: &Opts) -> Result<(), String> {
    let folder = folder_arg(o, 1)?;
    if o.pos.len() < 2 && too_broad(&folder) {
        return Err(format!(
            "not sharing {} without being asked: it's your whole home folder or a drive. Run \
             `wetwaresuite-share serve FOLDER` with the folder you mean (or `serve .` to share this one anyway)",
            folder.display()
        ));
    }
    let mut share = Share::start(config(o, folder)).map_err(|e| format!("can't share: {e}"))?;
    let _exposure = announce(&share, o, "Sharing", &format!("type it on the page, or `wetwaresuite-share receive {}`", share.code()));
    share.wait();
    Ok(())
}

fn sync_cmd(o: &Opts) -> Result<(), String> {
    let folder = PathBuf::from(o.pos.get(1).ok_or("usage: wetwaresuite-share sync FOLDER [CODE|LINK]")?);
    match o.pos.get(2) {
        None => {
            std::fs::create_dir_all(&folder).map_err(|e| format!("{}: {e}", folder.display()))?;
            let mut c = config(o, folder);
            c.writable = true;
            c.kind = "sync".into();
            let mut share = Share::start(c).map_err(|e| format!("can't host sync: {e}"))?;
            let hint = format!("on the other machine: wetwaresuite-share sync <folder> {}", share.code());
            let _exposure = announce(&share, o, "Hosting live sync of", &hint);
            println!("  Waiting for the other machine. Changes made there show up here, and the other way round.\n");
            share.wait();
            Ok(())
        }
        Some(peer) => {
            println!("{} {peer}...", if rand::looks_like_code(peer) { "Looking for" } else { "Connecting to" });
            let (target, info) = client::resolve(peer, o.code.as_deref()).map_err(|e| e.to_string())?;
            println!(
                "Syncing {} with {} on {} ({}). Press Ctrl+C to stop.",
                folder.display(),
                info.str_or("folder", "?"),
                info.str_or("host", "?"),
                target.base_url()
            );
            if !o.include_heavy {
                println!("  Skipping .git, node_modules, target, build, .venv, __pycache__ and .gitignore/.shareignore matches.");
            }
            let mut opts = sync::Options::new(&folder, target);
            opts.include_heavy = o.include_heavy;
            opts.log = Some(logger());
            let mut h = sync::start(opts).map_err(|e| format!("can't start sync: {e}"))?;
            let mut announced = false;
            while h.running() {
                std::thread::sleep(Duration::from_millis(200));
                let s = h.stats.lock().unwrap().clone();
                if s.in_sync && !announced {
                    println!("  In sync ({} file{}). Watching for changes...", s.files, if s.files == 1 { "" } else { "s" });
                    announced = true;
                } else if !s.in_sync {
                    announced = false;
                }
            }
            h.wait();
            Ok(())
        }
    }
}

fn send(o: &Opts) -> Result<(), String> {
    let path = PathBuf::from(o.pos.get(1).ok_or("usage: wetwaresuite-share send PATH [CODE|LINK]")?);
    if let Some(dest) = o.pos.get(2) {
        let (target, info) = client::resolve(dest, o.code.as_deref()).map_err(|e| e.to_string())?;
        println!("Sending {} to {} on {}...", path.display(), info.str_or("folder", "?"), info.str_or("host", "?"));
        let t = client::send_to(&target, &path, &|m| println!("  {m}")).map_err(|e| e.to_string())?;
        println!("Sent {} file(s), {}.", t.files, util::human_size(t.bytes));
        return Ok(());
    }
    let mut c = config(o, path);
    c.writable = false;
    c.kind = "send".into();
    let mut share = Share::start(c).map_err(|e| format!("can't send: {e}"))?;
    let hint = format!("on the other machine: wetwaresuite-share receive {}", share.code());
    let _exposure = announce(&share, o, "Sending", &hint);
    wait_for_done(&mut share, o.keep, "download")
}

fn wait_for_done(share: &mut Share, keep: bool, what: &str) -> Result<(), String> {
    if keep {
        share.wait();
        return Ok(());
    }
    println!("  Will stop after the first finished {what} (use --keep to stay up).\n");
    while share.shared.done_count.load(std::sync::atomic::Ordering::Relaxed) == 0 {
        std::thread::sleep(Duration::from_millis(200));
    }
    std::thread::sleep(Duration::from_millis(300)); // let the last response flush
    println!("Done.");
    share.stop();
    Ok(())
}

fn receive(o: &Opts) -> Result<(), String> {
    let dest = match &o.to {
        Some(d) => d.clone(),
        None => std::env::current_dir().map_err(|e| e.to_string())?,
    };
    if let Some(from) = o.pos.get(1) {
        println!("{} {from}...", if rand::looks_like_code(from) { "Looking for" } else { "Connecting to" });
        let (target, info) = client::resolve(from, o.code.as_deref()).map_err(|e| e.to_string())?;
        println!("Receiving {} from {}...", info.str_or("folder", "?"), info.str_or("host", "?"));
        let t = client::fetch(&target, "", &dest, &|m| println!("  {m}")).map_err(|e| e.to_string())?;
        println!("Saved {} file(s), {} to {}.", t.files, util::human_size(t.bytes), t.saved_to.display());
        return Ok(());
    }
    std::fs::create_dir_all(&dest).map_err(|e| format!("{}: {e}", dest.display()))?;
    let mut c = config(o, dest);
    c.writable = true;
    c.kind = "receive".into();
    let mut share = Share::start(c).map_err(|e| format!("can't receive: {e}"))?;
    let hint = format!("on the other machine: wetwaresuite-share send <path> {}", share.code());
    let _exposure = announce(&share, o, "Receiving into", &hint);
    wait_for_done(&mut share, o.keep, "upload")
}

fn peers(o: &Opts) -> Result<(), String> {
    let wait = Duration::from_secs_f64(o.wait.unwrap_or(3.0).clamp(0.2, 120.0));
    println!("Listening for shares on this network for {:.0}s...", wait.as_secs_f64());
    let found = discovery::listen(wait, None).map_err(|e| e.to_string())?;
    if found.is_empty() {
        println!("None found. Shares announce over UDP port {}; check you're on the same network and the firewall allows it.", share::DISCOVERY_PORT);
    }
    for p in found {
        println!(
            "  {:<20} {:<12} {:<8} {}{}",
            p.ann.folder,
            p.ann.host,
            p.ann.kind,
            p.base_url(),
            if p.ann.writable { "  (writable)" } else { "" }
        );
    }
    println!("Join one with its pairing code (shown where it was started), e.g. `wetwaresuite-share sync myproj CODE`.");
    Ok(())
}

fn run(args: &[String]) -> Result<(), String> {
    let o = parse(args)?;
    let cmd = o.pos.first().map(String::as_str).unwrap_or("serve");
    match cmd {
        "serve" | "share" => serve(&o),
        "sync" => sync_cmd(&o),
        "send" => send(&o),
        "receive" | "recv" => receive(&o),
        "peers" => peers(&o),
        "mcp" => mcp::run().map_err(|e| e.to_string()),
        "help" => {
            print!("{USAGE}");
            Ok(())
        }
        // a bare folder path or code is a natural thing to type
        other if std::path::Path::new(other).is_dir() => {
            let mut o2 = parse(args)?;
            o2.pos.insert(0, "serve".into());
            serve(&o2)
        }
        other if rand::looks_like_code(other) => {
            let mut o2 = parse(args)?;
            o2.pos.insert(0, "receive".into());
            receive(&o2)
        }
        other => Err(format!("unknown command {other:?}; run `wetwaresuite-share help`")),
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "--version" || a == "-V") {
        println!("wetwaresuite-share {VERSION}");
        return ExitCode::SUCCESS;
    }
    if args.iter().any(|a| a == "--help" || a == "-h") {
        print!("{USAGE}");
        return ExitCode::SUCCESS;
    }
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("wetwaresuite-share: {e}");
            if cfg!(windows) && args.is_empty() {
                // started by double-click: keep the window open so the message can be read
                eprintln!("press Enter to close");
                let _ = std::io::stdin().read_line(&mut String::new());
            }
            ExitCode::FAILURE
        }
    }
}
