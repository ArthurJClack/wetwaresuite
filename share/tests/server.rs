//! Starts real shares on 127.0.0.1 and talks raw HTTP to them.

use std::io::{Read, Write};
use std::net::{IpAddr, Ipv4Addr, TcpStream};
use std::path::PathBuf;

use share::http::Target;
use share::server::{Config, Share};

const TOKEN: &str = "testtokentesttokentesttokentest1";

fn tmp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("wetwaresuite-share-it-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn start(root: &std::path::Path, writable: bool) -> Share {
    let mut c = Config::new(root);
    c.bind = IpAddr::V4(Ipv4Addr::LOCALHOST);
    c.port = 0;
    c.writable = writable;
    c.announce = false;
    c.token = Some(TOKEN.into());
    c.code = Some("ABC234".into());
    c.max_upload = 1000;
    Share::start(c).unwrap()
}

struct Resp {
    status: u16,
    head: String,
    body: Vec<u8>,
}

fn raw(share: &Share, request: &str, body: &[u8]) -> Resp {
    let mut s = TcpStream::connect(("127.0.0.1", share.port())).unwrap();
    s.write_all(request.as_bytes()).unwrap();
    s.write_all(body).unwrap();
    let mut out = Vec::new();
    s.read_to_end(&mut out).unwrap();
    let split = out.windows(4).position(|w| w == b"\r\n\r\n").expect("response head");
    let head = String::from_utf8_lossy(&out[..split]).to_string();
    let status = head.split(' ').nth(1).unwrap().parse().unwrap();
    let mut body = out[split + 4..].to_vec();
    if head.to_ascii_lowercase().contains("transfer-encoding: chunked") {
        body = dechunk(&body);
    }
    Resp { status, head, body }
}

fn dechunk(mut b: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        let line_end = b.windows(2).position(|w| w == b"\r\n").unwrap();
        let n = usize::from_str_radix(std::str::from_utf8(&b[..line_end]).unwrap(), 16).unwrap();
        b = &b[line_end + 2..];
        if n == 0 {
            return out;
        }
        out.extend_from_slice(&b[..n]);
        b = &b[n + 2..];
    }
}

fn get(share: &Share, target: &str, extra: &str) -> Resp {
    raw(share, &format!("GET {target} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n{extra}\r\n"), b"")
}

fn bearer() -> String {
    format!("Authorization: Bearer {TOKEN}\r\n")
}

#[test]
fn token_is_required_everywhere() {
    let root = tmp("auth");
    std::fs::write(root.join("hello.txt"), "hello world").unwrap();
    let share = start(&root, false);

    // no token: login page for /, 401 for everything else
    let r = get(&share, "/", "");
    assert_eq!(r.status, 401);
    assert!(String::from_utf8_lossy(&r.body).contains("pairing code") || String::from_utf8_lossy(&r.body).contains("Pairing code"));
    for path in ["/api/list", "/api/info", "/f/hello.txt", "/zip/", "/api/tree", "/api/sync/manifest"] {
        assert_eq!(get(&share, path, "").status, 401, "{path}");
    }
    // wrong token
    assert_eq!(get(&share, "/f/hello.txt?k=nope", "").status, 401);
    assert_eq!(get(&share, "/f/hello.txt", "Authorization: Bearer nope\r\n").status, 401);

    // right token in the query: redirect that swaps it for a cookie
    let r = get(&share, &format!("/?k={TOKEN}"), "");
    assert_eq!(r.status, 302);
    let cookie_line = r.head.lines().find(|l| l.to_ascii_lowercase().starts_with("set-cookie:")).unwrap();
    assert!(cookie_line.contains("HttpOnly") && cookie_line.contains("SameSite=Strict"));
    let cookie = cookie_line.split_once(':').unwrap().1.trim().split(';').next().unwrap().to_string();
    let r = get(&share, "/f/hello.txt", &format!("Cookie: {cookie}\r\n"));
    assert_eq!(r.status, 200);
    assert_eq!(r.body, b"hello world");
    assert!(r.head.contains("Content-Security-Policy: sandbox"));
    // the UI page itself
    let r = get(&share, "/", &format!("Cookie: {cookie}\r\n"));
    assert_eq!(r.status, 200);
    assert!(String::from_utf8_lossy(&r.body).contains("<title>"));
    // bearer header
    let r = get(&share, "/api/info", &bearer());
    assert_eq!(r.status, 200);
    assert!(String::from_utf8_lossy(&r.body).contains("\"writable\":false"));
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn repeated_bad_tokens_get_blocked() {
    let root = tmp("block");
    let share = start(&root, false);
    let mut statuses = Vec::new();
    for _ in 0..12 {
        statuses.push(get(&share, "/api/info?k=wrong", "").status);
    }
    assert_eq!(&statuses[..9], &[401; 9]);
    assert_eq!(statuses[11], 429);
    // even the right token is refused while blocked
    assert_eq!(get(&share, "/api/info", &bearer()).status, 429);
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn path_attacks_are_refused() {
    let base = tmp("attacks");
    let root = base.join("root");
    std::fs::create_dir_all(&root).unwrap();
    std::fs::write(base.join("secret.txt"), "secret").unwrap();
    std::fs::write(root.join("ok.txt"), "ok").unwrap();
    #[cfg(unix)]
    std::os::unix::fs::symlink(base.join("secret.txt"), root.join("link.txt")).unwrap();
    let share = start(&root, true);
    for target in [
        "/f/../secret.txt",
        "/f/%2e%2e/secret.txt",
        "/f/..%2fsecret.txt",
        "/f/..%5csecret.txt",
        "/f/%2fetc%2fpasswd",
        "/f//etc/passwd",
        "/f/C:%5cWindows%5cwin.ini",
        "/f/%5c%5cserver%5cshare",
        "/f/ok.txt%00.png",
        "/f/CON",
        "/api/list?path=..",
        "/api/list?path=%2Fetc",
        "/api/tree?path=../",
        "/zip/..",
    ] {
        let r = get(&share, target, &bearer());
        assert!(matches!(r.status, 400 | 403 | 404), "{target} gave {}", r.status);
        assert!(!String::from_utf8_lossy(&r.body).contains("secret"), "{target} leaked");
    }
    #[cfg(unix)]
    assert_eq!(get(&share, "/f/link.txt", &bearer()).status, 403);
    // writes outside the root
    for target in ["/f/../evil.txt", "/f/%2e%2e%2fevil.txt", "/f/sub/..%2f..%2fevil.txt"] {
        let r = raw(&share, &format!("PUT {target} HTTP/1.1\r\nConnection: close\r\n{}Content-Length: 4\r\n\r\n", bearer()), b"evil");
        assert!(matches!(r.status, 400 | 403), "{target} gave {}", r.status);
    }
    assert!(!base.join("evil.txt").exists());
    assert_eq!(get(&share, "/f/ok.txt", &bearer()).body, b"ok");
    let _ = std::fs::remove_dir_all(&base);
}

#[test]
fn uploads_need_write_and_respect_limits() {
    let root = tmp("upload");
    let ro = start(&root, false);
    let put = |share: &Share, path: &str, headers: &str, body: &[u8]| {
        raw(
            share,
            &format!("PUT {path} HTTP/1.1\r\nConnection: close\r\n{headers}Content-Length: {}\r\n\r\n", body.len()),
            body,
        )
    };
    assert_eq!(put(&ro, "/f/a.txt", &bearer(), b"data").status, 403);
    drop(ro);

    let rw = start(&root, true);
    let r = put(&rw, "/f/dir/a%20b.txt", &bearer(), b"uploaded!");
    assert_eq!(r.status, 201, "{}", String::from_utf8_lossy(&r.body));
    assert_eq!(std::fs::read(root.join("dir/a b.txt")).unwrap(), b"uploaded!");
    assert!(String::from_utf8_lossy(&r.body).contains(&share::sha256::digest_hex(b"uploaded!")));
    // cookie auth without the X-Share header is refused (CSRF guard)
    let r = get(&rw, &format!("/api/info?k={TOKEN}"), "");
    let cookie = r.head.lines().find(|l| l.to_ascii_lowercase().starts_with("set-cookie:")).unwrap();
    let cookie = cookie.split_once(':').unwrap().1.trim().split(';').next().unwrap().to_string();
    assert_eq!(put(&rw, "/f/c.txt", &format!("Cookie: {cookie}\r\n"), b"x").status, 403);
    assert_eq!(put(&rw, "/f/c.txt", &format!("Cookie: {cookie}\r\nX-Share: 1\r\n"), b"x").status, 201);
    // size limit (1000 bytes in these tests)
    assert_eq!(put(&rw, "/f/big.bin", &bearer(), &[0u8; 1001]).status, 413);
    // a large refused body still gets its 413 (no reset from unread data)
    assert_eq!(put(&rw, "/f/big.bin", &bearer(), &vec![0u8; 3 << 20]).status, 413);
    assert!(!root.join("big.bin").exists());
    // If-Match guards against overwriting unseen changes
    assert_eq!(put(&rw, "/f/dir/a%20b.txt", &format!("{}If-Match: deadbeef\r\n", bearer()), b"new").status, 412);
    assert_eq!(put(&rw, "/f/dir/a%20b.txt", &format!("{}If-Match: none\r\n", bearer()), b"new").status, 412);
    let h = share::sha256::digest_hex(b"uploaded!");
    assert_eq!(put(&rw, "/f/dir/a%20b.txt", &format!("{}If-Match: {h}\r\n", bearer()), b"new").status, 201);
    // can't replace a folder with a file, or write into a file
    assert_eq!(put(&rw, "/f/dir", &bearer(), b"x").status, 409);
    assert_eq!(put(&rw, "/f/c.txt/x", &bearer(), b"x").status, 400);
    // mkdir, move, delete
    let post = |p: &str| raw(&rw, &format!("POST {p} HTTP/1.1\r\nConnection: close\r\n{}Content-Length: 0\r\n\r\n", bearer()), b"");
    assert_eq!(post("/api/mkdir?path=new%20folder").status, 201);
    assert!(root.join("new folder").is_dir());
    assert_eq!(post("/api/move?from=c.txt&to=new%20folder/c2.txt").status, 200);
    assert!(root.join("new folder/c2.txt").is_file());
    let del = raw(&rw, &format!("DELETE /f/new%20folder HTTP/1.1\r\nConnection: close\r\n{}\r\n", bearer()), b"");
    assert_eq!(del.status, 200);
    assert!(!root.join("new folder").exists());
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn downloads_ranges_listing_and_zip() {
    let root = tmp("download");
    std::fs::create_dir_all(root.join("docs/deep")).unwrap();
    std::fs::write(root.join("docs/a.txt"), "0123456789").unwrap();
    std::fs::write(root.join("docs/deep/b.md"), "# b").unwrap();
    let share = start(&root, false);
    let r = get(&share, "/f/docs/a.txt", &format!("{}Range: bytes=2-4\r\n", bearer()));
    assert_eq!(r.status, 206);
    assert_eq!(r.body, b"234");
    assert!(r.head.contains("Content-Range: bytes 2-4/10"));
    assert_eq!(get(&share, "/f/docs/a.txt", &format!("{}Range: bytes=50-\r\n", bearer())).status, 416);
    let r = get(&share, "/f/docs/a.txt?dl=1", &bearer());
    assert!(r.head.contains("attachment"));
    let r = get(&share, "/api/list?path=docs", &bearer());
    let j = share::json::Json::parse(&String::from_utf8_lossy(&r.body)).unwrap();
    let names: Vec<&str> = j.get("entries").unwrap().as_array().unwrap().iter().map(|e| e.str_or("name", "")).collect();
    assert!(names.contains(&"a.txt") && names.contains(&"deep"));
    let r = get(&share, "/zip/docs", &bearer());
    assert_eq!(r.status, 200);
    assert!(r.body.starts_with(b"PK\x03\x04"));
    let text = String::from_utf8_lossy(&r.body);
    assert!(text.contains("docs/a.txt") && text.contains("docs/deep/b.md") && text.contains("0123456789"));
    assert!(r.body.windows(4).any(|w| w == b"PK\x05\x06"));
    // keep-alive: two requests on one connection
    let mut s = TcpStream::connect(("127.0.0.1", share.port())).unwrap();
    let req = format!("GET /api/info HTTP/1.1\r\n{}\r\nGET /api/info HTTP/1.1\r\n{}Connection: close\r\n\r\n", bearer(), bearer());
    s.write_all(req.as_bytes()).unwrap();
    let mut out = String::new();
    s.read_to_string(&mut out).unwrap();
    assert_eq!(out.matches("HTTP/1.1 200 OK").count(), 2);
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn pairing_by_code() {
    let root = tmp("pair");
    let share = start(&root, false);
    let pair = |code: &str| {
        let body = format!("{{\"code\":\"{code}\"}}");
        raw(&share, &format!("POST /api/pair HTTP/1.1\r\nConnection: close\r\nContent-Length: {}\r\n\r\n", body.len()), body.as_bytes())
    };
    assert_eq!(pair("ZZZ999").status, 403);
    let r = pair("abc-234");
    assert_eq!(r.status, 200);
    assert!(String::from_utf8_lossy(&r.body).contains(TOKEN));
    // the client helper pairs with a bare link plus --code
    let base = format!("http://127.0.0.1:{}", share.port());
    let (t, info) = share::client::resolve(&base, Some("ABC234")).unwrap();
    assert_eq!(t.token, TOKEN);
    assert_eq!(info.str_or("app", ""), "wetwaresuite-share");
    // five wrong codes in a minute block the address
    for _ in 0..5 {
        pair("QQQQQQ");
    }
    assert_eq!(pair("ABC234").status, 429);
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn send_and_receive_round_trip() {
    let base = tmp("sendrecv");
    let src = base.join("project");
    std::fs::create_dir_all(src.join("sub")).unwrap();
    std::fs::write(src.join("readme.md"), "hi").unwrap();
    std::fs::write(src.join("sub/data.bin"), vec![7u8; 300]).unwrap();
    // receive: a writable drop box; send pushes into it
    let inbox = base.join("inbox");
    std::fs::create_dir_all(&inbox).unwrap();
    let mut c = Config::new(&inbox);
    c.bind = IpAddr::V4(Ipv4Addr::LOCALHOST);
    c.port = 0;
    c.writable = true;
    c.announce = false;
    let drop_box = Share::start(c).unwrap();
    let t = Target::parse(&drop_box.local_url()).unwrap();
    let sent = share::client::send_to(&t, &src, &|_| {}).unwrap();
    assert_eq!(sent.files, 2);
    assert_eq!(std::fs::read(inbox.join("project/sub/data.bin")).unwrap(), vec![7u8; 300]);
    assert_eq!(drop_box.shared.done_count.load(std::sync::atomic::Ordering::Relaxed), 1);

    // send: serve one file; fetch pulls it down
    let mut c = Config::new(src.join("readme.md"));
    c.bind = IpAddr::V4(Ipv4Addr::LOCALHOST);
    c.port = 0;
    c.announce = false;
    let one = Share::start(c).unwrap();
    let t = Target::parse(&one.local_url()).unwrap();
    let out = base.join("out");
    let got = share::client::fetch(&t, "", &out, &|_| {}).unwrap();
    assert_eq!(got.files, 1);
    assert_eq!(std::fs::read_to_string(out.join("readme.md")).unwrap(), "hi");
    // a single-file share exposes nothing else
    let list = share::client::list_remote(&t, "").unwrap();
    assert_eq!(list.get("entries").unwrap().as_array().unwrap().len(), 1);
    let mut cl = share::http::Client::new(t.clone());
    assert!(cl.fetch("GET", "/f/sub/data.bin", share::http::ReqBody::None).is_err());

    // folder fetch
    let mut c = Config::new(&src);
    c.bind = IpAddr::V4(Ipv4Addr::LOCALHOST);
    c.port = 0;
    c.announce = false;
    let folder = Share::start(c).unwrap();
    let t = Target::parse(&folder.local_url()).unwrap();
    let got = share::client::fetch(&t, "sub", &out, &|_| {}).unwrap();
    assert_eq!(got.files, 1);
    assert!(out.join("sub/data.bin").is_file());
    let got = share::client::fetch(&t, "", &out, &|_| {}).unwrap();
    assert_eq!(got.files, 2);
    assert!(out.join("project/readme.md").is_file());
    let _ = std::fs::remove_dir_all(&base);
}
