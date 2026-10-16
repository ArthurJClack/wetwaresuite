//! Two folders kept in sync through a real share on loopback.

use std::net::{IpAddr, Ipv4Addr};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use share::http::Target;
use share::server::{Config, Share};
use share::sync;

fn tmp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("achclwss-share-sync-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// Polls until `cond` holds; returns how long it took.
fn wait_for(what: &str, cond: impl Fn() -> bool) -> Duration {
    let start = Instant::now();
    while !cond() {
        assert!(start.elapsed() < Duration::from_secs(15), "timed out waiting for {what}");
        std::thread::sleep(Duration::from_millis(5));
    }
    start.elapsed()
}

fn read(p: &Path) -> Option<String> {
    std::fs::read_to_string(p).ok()
}

fn host(dir: &Path) -> Share {
    let mut c = Config::new(dir);
    c.bind = IpAddr::V4(Ipv4Addr::LOCALHOST);
    c.port = 0;
    c.writable = true;
    c.announce = false;
    c.kind = "sync".into();
    Share::start(c).unwrap()
}

fn options(dir: &Path, share: &Share, state: &Path) -> sync::Options {
    let mut o = sync::Options::new(dir, Target::parse(&share.local_url()).unwrap());
    o.poll = Duration::from_millis(100);
    o.host = "laptop".into();
    o.state_dir = Some(state.to_path_buf());
    o
}

#[test]
fn live_two_way_sync() {
    let base = tmp("live");
    let (a, b, state) = (base.join("host"), base.join("peer"), base.join("state"));
    std::fs::create_dir_all(a.join("src")).unwrap();
    std::fs::create_dir_all(b.join("node_modules/pkg")).unwrap();
    std::fs::write(a.join("src/main.rs"), "fn main() {}\n").unwrap();
    std::fs::write(a.join("same.txt"), "same").unwrap();
    std::fs::write(b.join("same.txt"), "same").unwrap();
    std::fs::write(b.join("notes.md"), "from peer").unwrap();
    std::fs::write(b.join("node_modules/pkg/index.js"), "heavy").unwrap();
    std::fs::write(b.join(".gitignore"), "*.log\n").unwrap();
    std::fs::write(b.join("debug.log"), "noise").unwrap();

    let share = host(&a);
    let mut s = sync::start(options(&b, &share, &state)).unwrap();

    // initial sync by manifest diff
    let t = wait_for("initial sync", || {
        read(&b.join("src/main.rs")).is_some() && read(&a.join("notes.md")).is_some() && read(&a.join(".gitignore")).is_some()
    });
    eprintln!("initial sync: {t:?}");
    assert!(!a.join("node_modules").exists(), "heavy folders must not sync");
    assert!(!a.join("debug.log").exists(), ".gitignore matches must not sync");

    let mut times = Vec::new();
    // create here -> there
    std::fs::write(b.join("src/lib.rs"), "pub fn x() {}\n").unwrap();
    times.push(("create peer->host", wait_for("create b->a", || read(&a.join("src/lib.rs")).as_deref() == Some("pub fn x() {}\n"))));
    // create there -> here, in a new folder
    std::fs::create_dir_all(a.join("assets/img")).unwrap();
    std::fs::write(a.join("assets/img/logo.svg"), "<svg/>").unwrap();
    times.push(("create host->peer", wait_for("create a->b", || read(&b.join("assets/img/logo.svg")).is_some())));
    // modify both ways
    std::fs::write(a.join("src/main.rs"), "fn main() { println!(\"hi\"); }\n").unwrap();
    times.push((
        "modify host->peer",
        wait_for("modify a->b", || read(&b.join("src/main.rs")).is_some_and(|s| s.contains("hi"))),
    ));
    std::fs::write(b.join("notes.md"), "edited on the peer").unwrap();
    times.push((
        "modify peer->host",
        wait_for("modify b->a", || read(&a.join("notes.md")).as_deref() == Some("edited on the peer")),
    ));
    // delete both ways (and empty folders go with the last file)
    std::fs::remove_file(b.join("assets/img/logo.svg")).unwrap();
    times.push(("delete peer->host", wait_for("delete b->a", || !a.join("assets/img/logo.svg").exists())));
    // the file goes first and its emptied folders right after, so wait for those too
    wait_for("empty folders left by a sync delete are removed", || !a.join("assets").exists());
    std::fs::remove_file(a.join("src/lib.rs")).unwrap();
    times.push(("delete host->peer", wait_for("delete a->b", || !b.join("src/lib.rs").exists())));
    // a 1 MB file
    let big: Vec<u8> = (0..1_000_000u32).map(|i| (i % 253) as u8).collect();
    std::fs::write(b.join("blob.bin"), &big).unwrap();
    times.push(("1 MB peer->host", wait_for("big b->a", || std::fs::read(a.join("blob.bin")).is_ok_and(|d| d == big))));
    for (what, t) in &times {
        eprintln!("{what}: {t:?}");
    }
    // the ignored files stayed where they were
    assert!(b.join("node_modules/pkg/index.js").exists() && b.join("debug.log").exists());
    wait_for("in sync", || s.stats.lock().unwrap().in_sync);
    s.stop();

    // conflict: both sides edit the same file while sync is off
    std::fs::write(a.join("same.txt"), "host edit").unwrap();
    std::fs::write(b.join("same.txt"), "peer edit").unwrap();
    // and a delete on one side while off must not come back
    std::fs::remove_file(b.join("blob.bin")).unwrap();
    let mut s = sync::start(options(&b, &share, &state)).unwrap();
    wait_for("conflict copy", || {
        read(&a.join("same.conflict-laptop.txt")).as_deref() == Some("peer edit")
            && read(&b.join("same.conflict-laptop.txt")).as_deref() == Some("peer edit")
    });
    wait_for("host version wins the name", || read(&b.join("same.txt")).as_deref() == Some("host edit"));
    wait_for("delete while offline", || !a.join("blob.bin").exists());
    assert_eq!(read(&a.join("same.txt")).as_deref(), Some("host edit"));
    assert!(s.stats.lock().unwrap().conflicts >= 1);
    s.stop();
    drop(share);
    let _ = std::fs::remove_dir_all(&base);
}

#[test]
fn refuses_read_only_hosts() {
    let base = tmp("ro");
    let mut c = Config::new(&base);
    c.bind = IpAddr::V4(Ipv4Addr::LOCALHOST);
    c.port = 0;
    c.announce = false;
    let share = Share::start(c).unwrap();
    let o = sync::Options::new(base.join("other"), Target::parse(&share.local_url()).unwrap());
    let err = sync::start(o).err().expect("read-only host must be refused");
    assert!(err.to_string().contains("read-only"));
    let _ = std::fs::remove_dir_all(&base);
}
