//! Block-delta sync through a real share on loopback: small edits to a big
//! file move only a few KB each way, a damaged delta falls back to the whole
//! file, and a peer with deltas off keeps sending whole files.

use std::net::{IpAddr, Ipv4Addr};
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use share::delta;
use share::http::Target;
use share::server::{Config, Share};
use share::sync;

fn tmp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("wetwaresuite-share-delta-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn wait_for(what: &str, cond: impl Fn() -> bool) {
    let start = Instant::now();
    while !cond() {
        assert!(start.elapsed() < Duration::from_secs(60), "timed out waiting for {what}");
        std::thread::sleep(Duration::from_millis(10));
    }
}

fn random(n: usize, seed: u64) -> Vec<u8> {
    let mut x = seed | 1;
    (0..n)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            (x >> 24) as u8
        })
        .collect()
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

/// Writes via a temp file + rename, so a scan never sees a half-written file.
fn put(p: &Path, data: &[u8]) {
    let t = p.parent().unwrap().parent().unwrap().join("tmp-write");
    std::fs::write(&t, data).unwrap();
    std::fs::rename(&t, p).unwrap();
}

fn same(p: &Path, want: &[u8]) -> bool {
    std::fs::read(p).is_ok_and(|d| d == want)
}

/// Bytes moved (up + down) and delta files so far.
fn moved(s: &sync::SyncHandle) -> (u64, u64, u64) {
    let st = s.stats.lock().unwrap();
    (st.bytes_up + st.bytes_down, st.delta_files, st.delta_fallbacks)
}

#[test]
fn big_file_edits_move_only_changed_blocks() {
    let base = tmp("edits");
    let (a, b, state) = (base.join("host"), base.join("peer"), base.join("state"));
    std::fs::create_dir_all(&a).unwrap();
    std::fs::create_dir_all(&b).unwrap();
    let mut data = random(5 << 20, 99);
    put(&a.join("big.bin"), &data);
    let share = host(&a);
    let mut s = sync::start(options(&b, &share, &state)).unwrap();
    wait_for("first copy", || same(&b.join("big.bin"), &data));
    wait_for("in sync", || s.stats.lock().unwrap().in_sync);
    let (first, _, _) = moved(&s);
    assert!(first >= 5 << 20, "first copy is whole");

    // host edits (insert in the middle) -> peer pulls a delta
    data.splice(2_000_000..2_000_000, random(3000, 5));
    put(&a.join("big.bin"), &data);
    wait_for("insert pulled", || same(&b.join("big.bin"), &data));
    wait_for("delta counted", || moved(&s).1 == 1);
    let (after_pull, _, _) = moved(&s);
    assert!(after_pull - first < 200_000, "a 3 KB insert moved {} bytes", after_pull - first);

    // peer edits (delete in the middle, append at the end) -> peer pushes a delta
    data.drain(4_000_000..4_010_000);
    data.extend_from_slice(b"appended on the peer");
    put(&b.join("big.bin"), &data);
    wait_for("edit pushed", || same(&a.join("big.bin"), &data));
    wait_for("delta counted", || moved(&s).1 == 2);
    let (after_push, _, fallbacks) = moved(&s);
    assert!(after_push - after_pull < 200_000, "a delete + append moved {} bytes", after_push - after_pull);
    assert_eq!(fallbacks, 0);

    // damaged deltas are caught and the whole file goes instead, both ways
    delta::CORRUPT_FOR_TEST.store(true, Ordering::Relaxed);
    data[100] ^= 0xff;
    put(&a.join("big.bin"), &data);
    wait_for("pull fallback", || same(&b.join("big.bin"), &data) && moved(&s).2 == 1);
    data[3_000_000] ^= 0xff;
    put(&b.join("big.bin"), &data);
    wait_for("push fallback", || same(&a.join("big.bin"), &data) && moved(&s).2 == 2);
    delta::CORRUPT_FOR_TEST.store(false, Ordering::Relaxed);
    assert_eq!(moved(&s).1, 2, "damaged deltas don't count as deltas");
    assert!(same(&a.join("big.bin"), &data) && same(&b.join("big.bin"), &data));
    let leftovers: Vec<_> = std::fs::read_dir(&a)
        .unwrap()
        .chain(std::fs::read_dir(&b).unwrap())
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n != "big.bin")
        .collect();
    assert!(leftovers.is_empty(), "temp files left behind: {leftovers:?}");
    s.stop();

    // a peer with deltas off (like an older version) sends whole files
    let mut o = options(&b, &share, &state);
    o.delta = false;
    let mut s = sync::start(o).unwrap();
    wait_for("in sync", || s.stats.lock().unwrap().in_sync);
    data[10] ^= 0xff;
    put(&b.join("big.bin"), &data);
    wait_for("whole push", || same(&a.join("big.bin"), &data) && moved(&s).0 >= data.len() as u64);
    assert_eq!(moved(&s).1, 0);
    s.stop();
    drop(share);
    let _ = std::fs::remove_dir_all(&base);
}
