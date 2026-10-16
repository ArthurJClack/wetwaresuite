//! Just enough HTTP/1.1 for the share: a request parser and response writer
//! for the server (keep-alive, Content-Length and chunked responses, ranges),
//! and a keep-alive client for sync and the CLI.

use std::io::{self, BufRead, BufReader, BufWriter, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::path::Path;
use std::time::Duration;

use crate::util;

const MAX_HEAD: usize = 32 * 1024;
const MAX_HEADERS: usize = 100;

#[derive(Debug, Clone)]
pub struct Request {
    pub method: String,
    /// Decoded path, without the query string.
    pub path: String,
    pub query: Vec<(String, String)>,
    /// Lowercased names.
    pub headers: Vec<(String, String)>,
    pub content_length: u64,
    pub peer: SocketAddr,
    pub http10: bool,
}

impl Request {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k == name).map(|(_, v)| v.as_str())
    }

    pub fn query(&self, name: &str) -> Option<&str> {
        self.query.iter().find(|(k, _)| k == name).map(|(_, v)| v.as_str())
    }

    pub fn cookie(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .filter(|(k, _)| k == "cookie")
            .flat_map(|(_, v)| v.split(';'))
            .filter_map(|c| c.trim().split_once('='))
            .find(|(k, _)| *k == name)
            .map(|(_, v)| v)
    }

    pub fn wants_keep_alive(&self) -> bool {
        let conn = self.header("connection").unwrap_or("").to_ascii_lowercase();
        if self.http10 {
            conn.contains("keep-alive")
        } else {
            !conn.contains("close")
        }
    }
}

#[derive(Debug)]
pub enum ParseError {
    Io(io::Error),
    Bad(&'static str),
    TooLarge,
}

impl From<io::Error> for ParseError {
    fn from(e: io::Error) -> Self {
        ParseError::Io(e)
    }
}

fn read_line_limited<R: BufRead>(r: &mut R, budget: &mut usize) -> Result<Option<String>, ParseError> {
    let mut buf = Vec::new();
    loop {
        let available = match r.fill_buf() {
            Ok(b) => b,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e.into()),
        };
        if available.is_empty() {
            return if buf.is_empty() { Ok(None) } else { Err(ParseError::Bad("truncated head")) };
        }
        let (take, done) = match available.iter().position(|b| *b == b'\n') {
            Some(i) => (i + 1, true),
            None => (available.len(), false),
        };
        if take > *budget {
            return Err(ParseError::TooLarge);
        }
        *budget -= take;
        buf.extend_from_slice(&available[..take]);
        r.consume(take);
        if done {
            while buf.last().is_some_and(|b| *b == b'\n' || *b == b'\r') {
                buf.pop();
            }
            return String::from_utf8(buf).map(Some).map_err(|_| ParseError::Bad("non-UTF-8 head"));
        }
    }
}

/// Reads the next request head. `Ok(None)` means the client closed cleanly.
pub fn read_request<R: BufRead>(r: &mut R, peer: SocketAddr) -> Result<Option<Request>, ParseError> {
    let mut budget = MAX_HEAD;
    let line = loop {
        match read_line_limited(r, &mut budget)? {
            None => return Ok(None),
            Some(l) if l.is_empty() => continue, // tolerate stray CRLFs between requests
            Some(l) => break l,
        }
    };
    let mut parts = line.split(' ');
    let method = parts.next().ok_or(ParseError::Bad("no method"))?.to_string();
    let target = parts.next().ok_or(ParseError::Bad("no target"))?;
    let version = parts.next().ok_or(ParseError::Bad("no version"))?;
    if !version.starts_with("HTTP/1.") || parts.next().is_some() {
        return Err(ParseError::Bad("bad request line"));
    }
    if !method.bytes().all(|b| b.is_ascii_uppercase()) || method.is_empty() {
        return Err(ParseError::Bad("bad method"));
    }
    let (raw_path, raw_query) = target.split_once('?').unwrap_or((target, ""));
    if !raw_path.starts_with('/') {
        return Err(ParseError::Bad("target must start with /"));
    }
    let path = util::percent_decode(raw_path).ok_or(ParseError::Bad("bad percent-encoding"))?;
    let query = util::parse_query(raw_query);
    let mut headers = Vec::new();
    loop {
        let l = read_line_limited(r, &mut budget)?.ok_or(ParseError::Bad("truncated head"))?;
        if l.is_empty() {
            break;
        }
        if headers.len() >= MAX_HEADERS {
            return Err(ParseError::TooLarge);
        }
        let (k, v) = l.split_once(':').ok_or(ParseError::Bad("bad header"))?;
        headers.push((k.trim().to_ascii_lowercase(), v.trim().to_string()));
    }
    let mut req = Request { method, path, query, headers, content_length: 0, peer, http10: version == "HTTP/1.0" };
    if req.header("transfer-encoding").is_some() {
        return Err(ParseError::Bad("chunked request bodies are not supported; send Content-Length"));
    }
    if let Some(cl) = req.header("content-length") {
        req.content_length = cl.parse().map_err(|_| ParseError::Bad("bad content-length"))?;
    }
    Ok(Some(req))
}

pub fn status_text(code: u16) -> &'static str {
    match code {
        200 => "OK",
        201 => "Created",
        204 => "No Content",
        206 => "Partial Content",
        302 => "Found",
        304 => "Not Modified",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        409 => "Conflict",
        411 => "Length Required",
        412 => "Precondition Failed",
        413 => "Payload Too Large",
        416 => "Range Not Satisfiable",
        429 => "Too Many Requests",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        _ => "Unknown",
    }
}

pub type StreamFn = Box<dyn FnOnce(&mut dyn Write) -> io::Result<()> + Send>;

pub enum Body {
    Empty,
    Bytes(Vec<u8>),
    /// A file region: (file, start, length)
    File(std::fs::File, u64, u64),
    /// Unknown length, sent chunked.
    Stream(StreamFn),
}

pub struct Response {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Body,
}

impl Response {
    pub fn new(status: u16) -> Response {
        Response { status, headers: Vec::new(), body: Body::Empty }
    }

    pub fn bytes(status: u16, content_type: &str, body: Vec<u8>) -> Response {
        Response::new(status).header("Content-Type", content_type).with_body(Body::Bytes(body))
    }

    pub fn json(status: u16, value: &crate::json::Json) -> Response {
        Response::bytes(status, "application/json; charset=utf-8", value.dump().into_bytes())
            .header("Cache-Control", "no-store")
    }

    pub fn text(status: u16, text: &str) -> Response {
        Response::bytes(status, "text/plain; charset=utf-8", text.as_bytes().to_vec())
    }

    pub fn error(status: u16, message: &str) -> Response {
        Response::json(status, &crate::json::Json::obj().with("error", message))
    }

    pub fn redirect(location: &str) -> Response {
        Response::new(302).header("Location", location)
    }

    pub fn header(mut self, k: &str, v: &str) -> Response {
        self.headers.push((k.to_string(), v.to_string()));
        self
    }

    pub fn with_body(mut self, body: Body) -> Response {
        self.body = body;
        self
    }
}

struct Chunked<W: Write>(W);

impl<W: Write> Write for Chunked<W> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        write!(self.0, "{:x}\r\n", buf.len())?;
        self.0.write_all(buf)?;
        self.0.write_all(b"\r\n")?;
        Ok(buf.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        self.0.flush()
    }
}

/// Writes a response; returns the number of body bytes sent.
pub fn write_response(stream: &TcpStream, resp: Response, head_only: bool, keep_alive: bool) -> io::Result<u64> {
    let mut w = BufWriter::with_capacity(64 * 1024, stream);
    write!(w, "HTTP/1.1 {} {}\r\n", resp.status, status_text(resp.status))?;
    for (k, v) in &resp.headers {
        write!(w, "{k}: {v}\r\n")?;
    }
    let mut sent = 0u64;
    let conn = if keep_alive { "keep-alive" } else { "close" };
    match resp.body {
        Body::Empty => write!(w, "Content-Length: 0\r\nConnection: {conn}\r\n\r\n")?,
        Body::Bytes(b) => {
            write!(w, "Content-Length: {}\r\nConnection: {conn}\r\n\r\n", b.len())?;
            if !head_only {
                w.write_all(&b)?;
                sent = b.len() as u64;
            }
        }
        Body::File(mut f, start, len) => {
            write!(w, "Content-Length: {len}\r\nConnection: {conn}\r\n\r\n")?;
            if !head_only {
                use std::io::Seek;
                f.seek(io::SeekFrom::Start(start))?;
                w.flush()?;
                // copy straight into the socket; std uses sendfile/splice where it can
                let mut s = stream;
                sent = io::copy(&mut f.take(len), &mut s)?;
                if sent < len {
                    return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "file shrank while sending"));
                }
            }
        }
        Body::Stream(f) => {
            write!(w, "Transfer-Encoding: chunked\r\nConnection: {conn}\r\n\r\n")?;
            if !head_only {
                let mut counter = Counter { inner: BufWriter::with_capacity(128 * 1024, Chunked(&mut w)), n: 0 };
                f(&mut counter)?;
                counter.inner.flush()?;
                sent = counter.n;
                drop(counter);
                w.write_all(b"0\r\n\r\n")?;
            }
        }
    }
    w.flush()?;
    Ok(sent)
}

struct Counter<W: Write> {
    inner: W,
    n: u64,
}

impl<W: Write> Write for Counter<W> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let n = self.inner.write(buf)?;
        self.n += n as u64;
        Ok(n)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()
    }
}

/// Parses `bytes=a-b`, `bytes=a-` or `bytes=-n` against a length. Multiple
/// ranges are not supported (the whole file is sent instead).
pub fn parse_range(header: &str, len: u64) -> Option<Result<(u64, u64), ()>> {
    let spec = header.trim().strip_prefix("bytes=")?;
    if spec.contains(',') {
        return None;
    }
    let (a, b) = spec.split_once('-')?;
    let (start, end) = if a.is_empty() {
        let n: u64 = b.parse().ok()?;
        if n == 0 {
            return Some(Err(()));
        }
        (len.saturating_sub(n), len.saturating_sub(1))
    } else {
        let start: u64 = a.parse().ok()?;
        let end: u64 = if b.is_empty() { len.saturating_sub(1) } else { b.parse::<u64>().ok()?.min(len.saturating_sub(1)) };
        (start, end)
    };
    if len == 0 || start > end || start >= len {
        return Some(Err(()));
    }
    Some(Ok((start, end - start + 1)))
}

// ---------------------------------------------------------------- client

/// Where a share lives, parsed from `http://host:port/...?k=token`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Target {
    pub host: String,
    pub port: u16,
    pub token: String,
}

impl Target {
    pub fn parse(url: &str) -> Result<Target, String> {
        let url = url.trim();
        if url.starts_with("https://") {
            return Err("https share links (cloudflared tunnels) are for browsers; for sync, send and fetch use \
                        the LAN or tailscale http:// link, or the pairing code"
                .into());
        }
        let rest = url.strip_prefix("http://").unwrap_or(url);
        let (hostport, tail) = match rest.find(['/', '?', '#']) {
            Some(i) => (&rest[..i], &rest[i..]),
            None => (rest, ""),
        };
        let (host, port) = if let Some(h) = hostport.strip_prefix('[') {
            let (h, p) = h.split_once(']').ok_or("bad IPv6 address")?;
            (h.to_string(), p.strip_prefix(':').unwrap_or(""))
        } else {
            match hostport.rsplit_once(':') {
                Some((h, p)) => (h.to_string(), p),
                None => (hostport.to_string(), ""),
            }
        };
        if host.is_empty() {
            return Err(format!("no host in {url:?}"));
        }
        let port = if port.is_empty() { 80 } else { port.parse().map_err(|_| format!("bad port in {url:?}"))? };
        let query = tail.split_once('?').map(|(_, q)| q.split('#').next().unwrap_or("")).unwrap_or("");
        let token = util::parse_query(query).into_iter().find(|(k, _)| k == "k").map(|(_, v)| v).unwrap_or_default();
        Ok(Target { host, port, token })
    }

    pub fn base_url(&self) -> String {
        if self.host.contains(':') {
            format!("http://[{}]:{}", self.host, self.port)
        } else {
            format!("http://{}:{}", self.host, self.port)
        }
    }

    pub fn url(&self) -> String {
        format!("{}/?k={}", self.base_url(), self.token)
    }
}

pub enum ReqBody<'a> {
    None,
    Bytes(&'a [u8]),
    File(&'a Path),
}

pub struct ClientResponse {
    pub status: u16,
    pub headers: Vec<(String, String)>,
}

impl ClientResponse {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k == name).map(|(_, v)| v.as_str())
    }
}

/// A keep-alive HTTP client bound to one share. Every request carries the
/// token as a bearer header.
pub struct Client {
    pub target: Target,
    conn: Option<BufReader<TcpStream>>,
    pub timeout: Duration,
}

impl Client {
    pub fn new(target: Target) -> Client {
        Client { target, conn: None, timeout: Duration::from_secs(30) }
    }

    fn connect(&self) -> io::Result<BufReader<TcpStream>> {
        let addrs: Vec<SocketAddr> = (self.target.host.as_str(), self.target.port).to_socket_addrs()?.collect();
        let mut last = io::Error::new(io::ErrorKind::NotFound, format!("can't resolve {}", self.target.host));
        for a in addrs {
            match TcpStream::connect_timeout(&a, Duration::from_secs(5)) {
                Ok(s) => {
                    s.set_nodelay(true)?;
                    s.set_read_timeout(Some(self.timeout))?;
                    s.set_write_timeout(Some(self.timeout))?;
                    return Ok(BufReader::with_capacity(128 * 1024, s));
                }
                Err(e) => last = e,
            }
        }
        Err(last)
    }

    /// Sends a request and streams the response body into `sink`.
    pub fn call(
        &mut self,
        method: &str,
        target: &str,
        headers: &[(&str, &str)],
        body: ReqBody,
        sink: &mut dyn Write,
    ) -> io::Result<ClientResponse> {
        // a pooled connection may have been closed by the server; retry once on a fresh one
        let reused = self.conn.is_some();
        match self.try_call(method, target, headers, &body, sink) {
            Err(e) if reused && is_stale(&e) => {
                self.conn = None;
                self.try_call(method, target, headers, &body, sink)
            }
            r => r,
        }
    }

    fn try_call(
        &mut self,
        method: &str,
        target: &str,
        headers: &[(&str, &str)],
        body: &ReqBody,
        sink: &mut dyn Write,
    ) -> io::Result<ClientResponse> {
        if self.conn.is_none() {
            self.conn = Some(self.connect()?);
        }
        let result = self.exchange(method, target, headers, body, sink);
        if result.is_err() {
            self.conn = None;
        }
        result
    }

    fn exchange(
        &mut self,
        method: &str,
        target: &str,
        headers: &[(&str, &str)],
        body: &ReqBody,
        sink: &mut dyn Write,
    ) -> io::Result<ClientResponse> {
        let host = self.target.host.clone();
        let port = self.target.port;
        let token = self.target.token.clone();
        let conn = self.conn.as_mut().expect("connected");
        let (len, mut file) = match body {
            ReqBody::None => (0, None),
            ReqBody::Bytes(b) => (b.len() as u64, None),
            ReqBody::File(p) => {
                let f = std::fs::File::open(p)?;
                (f.metadata()?.len(), Some(f))
            }
        };
        {
            let mut w = BufWriter::with_capacity(64 * 1024, conn.get_ref());
            write!(w, "{method} {target} HTTP/1.1\r\nHost: {host}:{port}\r\nAuthorization: Bearer {token}\r\n")?;
            for (k, v) in headers {
                write!(w, "{k}: {v}\r\n")?;
            }
            write!(w, "Content-Length: {len}\r\n\r\n")?;
            match body {
                ReqBody::Bytes(b) => w.write_all(b)?,
                ReqBody::File(_) => {
                    if let Some(f) = file.as_mut() {
                        let n = io::copy(&mut f.take(len), &mut w)?;
                        if n != len {
                            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "file changed while uploading"));
                        }
                    }
                }
                ReqBody::None => {}
            }
            w.flush()?;
        }
        let mut budget = MAX_HEAD;
        let status_line = read_line_limited(conn, &mut budget)
            .map_err(perr)?
            .ok_or_else(|| io::Error::new(io::ErrorKind::ConnectionAborted, "server closed the connection"))?;
        let status: u16 = status_line
            .split(' ')
            .nth(1)
            .and_then(|s| s.parse().ok())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, format!("bad status line {status_line:?}")))?;
        let mut hdrs = Vec::new();
        loop {
            let l = read_line_limited(conn, &mut budget)
                .map_err(perr)?
                .ok_or_else(|| io::Error::new(io::ErrorKind::UnexpectedEof, "truncated response"))?;
            if l.is_empty() {
                break;
            }
            if let Some((k, v)) = l.split_once(':') {
                hdrs.push((k.trim().to_ascii_lowercase(), v.trim().to_string()));
            }
        }
        let resp = ClientResponse { status, headers: hdrs };
        let close = resp.header("connection").is_some_and(|c| c.eq_ignore_ascii_case("close"));
        if method == "HEAD" || status == 204 || status == 304 {
            // no body
        } else if resp.header("transfer-encoding").is_some_and(|t| t.eq_ignore_ascii_case("chunked")) {
            loop {
                let mut b = MAX_HEAD;
                let size_line = read_line_limited(conn, &mut b)
                    .map_err(perr)?
                    .ok_or_else(|| io::Error::new(io::ErrorKind::UnexpectedEof, "truncated chunk"))?;
                let size = u64::from_str_radix(size_line.split(';').next().unwrap_or("").trim(), 16)
                    .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "bad chunk size"))?;
                if size == 0 {
                    // trailers
                    loop {
                        let l = read_line_limited(conn, &mut b).map_err(perr)?.unwrap_or_default();
                        if l.is_empty() {
                            break;
                        }
                    }
                    break;
                }
                let n = io::copy(&mut conn.by_ref().take(size), sink)?;
                if n != size {
                    return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "truncated chunk"));
                }
                let mut crlf = [0u8; 2];
                conn.read_exact(&mut crlf)?;
            }
        } else if let Some(cl) = resp.header("content-length") {
            let n: u64 = cl.parse().map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "bad content-length"))?;
            let got = io::copy(&mut conn.by_ref().take(n), sink)?;
            if got != n {
                return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "truncated body"));
            }
        } else {
            io::copy(conn, sink)?;
            self.conn = None;
            return Ok(resp);
        }
        if close {
            self.conn = None;
        }
        Ok(resp)
    }

    /// GET/POST/... returning the body as bytes; non-2xx becomes an error
    /// carrying the server's message.
    pub fn fetch(&mut self, method: &str, target: &str, body: ReqBody) -> io::Result<Vec<u8>> {
        let mut out = Vec::new();
        let resp = self.call(method, target, &[], body, &mut out)?;
        if !(200..300).contains(&resp.status) {
            return Err(http_error(resp.status, &out));
        }
        Ok(out)
    }

    pub fn fetch_json(&mut self, method: &str, target: &str, body: ReqBody) -> io::Result<crate::json::Json> {
        let bytes = self.fetch(method, target, body)?;
        crate::json::Json::parse(&String::from_utf8_lossy(&bytes)).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
    }
}

/// Builds an error from a non-2xx response, using the JSON `error` field
/// when there is one.
pub fn http_error(status: u16, body: &[u8]) -> io::Error {
    let text = String::from_utf8_lossy(body);
    let msg = crate::json::Json::parse(&text)
        .ok()
        .and_then(|j| j.get("error").and_then(|e| e.as_str()).map(str::to_string))
        .unwrap_or_else(|| text.trim().chars().take(200).collect());
    let kind = match status {
        401 | 403 => io::ErrorKind::PermissionDenied,
        404 => io::ErrorKind::NotFound,
        _ => io::ErrorKind::Other,
    };
    io::Error::new(kind, format!("HTTP {status}: {msg}"))
}

fn is_stale(e: &io::Error) -> bool {
    matches!(
        e.kind(),
        io::ErrorKind::ConnectionReset
            | io::ErrorKind::ConnectionAborted
            | io::ErrorKind::BrokenPipe
            | io::ErrorKind::UnexpectedEof
    )
}

fn perr(e: ParseError) -> io::Error {
    match e {
        ParseError::Io(e) => e,
        ParseError::Bad(m) => io::Error::new(io::ErrorKind::InvalidData, m),
        ParseError::TooLarge => io::Error::new(io::ErrorKind::InvalidData, "response head too large"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn peer() -> SocketAddr {
        "127.0.0.1:1".parse().unwrap()
    }

    #[test]
    fn parses_request() {
        let raw = b"GET /f/a%20b.txt?k=tok&x=1 HTTP/1.1\r\nHost: h\r\nCookie: a=1; wetwaresuite_share=zz\r\n\r\nGET / HTTP/1.0\r\n\r\n";
        let mut r = BufReader::new(&raw[..]);
        let req = read_request(&mut r, peer()).unwrap().unwrap();
        assert_eq!(req.method, "GET");
        assert_eq!(req.path, "/f/a b.txt");
        assert_eq!(req.query("k"), Some("tok"));
        assert_eq!(req.header("host"), Some("h"));
        assert_eq!(req.cookie("wetwaresuite_share"), Some("zz"));
        assert!(req.wants_keep_alive());
        let req2 = read_request(&mut r, peer()).unwrap().unwrap();
        assert!(req2.http10 && !req2.wants_keep_alive());
        assert!(read_request(&mut r, peer()).unwrap().is_none());
    }

    #[test]
    fn rejects_bad_requests() {
        for raw in [
            &b"GET\r\n\r\n"[..],
            b"GET x HTTP/1.1\r\n\r\n",
            b"GET / SPDY\r\n\r\n",
            b"get / HTTP/1.1\r\n\r\n",
            b"GET / HTTP/1.1\r\nbad header\r\n\r\n",
            b"PUT / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
            b"GET /%zz HTTP/1.1\r\n\r\n",
        ] {
            let mut r = BufReader::new(raw);
            assert!(read_request(&mut r, peer()).is_err(), "{:?}", String::from_utf8_lossy(raw));
        }
        let huge = format!("GET / HTTP/1.1\r\nX: {}\r\n\r\n", "a".repeat(40_000));
        assert!(matches!(read_request(&mut BufReader::new(huge.as_bytes()), peer()), Err(ParseError::TooLarge)));
    }

    #[test]
    fn ranges() {
        assert_eq!(parse_range("bytes=0-99", 1000), Some(Ok((0, 100))));
        assert_eq!(parse_range("bytes=900-", 1000), Some(Ok((900, 100))));
        assert_eq!(parse_range("bytes=-10", 1000), Some(Ok((990, 10))));
        assert_eq!(parse_range("bytes=5-5000", 1000), Some(Ok((5, 995))));
        assert_eq!(parse_range("bytes=1000-", 1000), Some(Err(())));
        assert_eq!(parse_range("bytes=0-1,4-5", 1000), None);
        assert_eq!(parse_range("items=0-1", 1000), None);
    }

    #[test]
    fn targets() {
        let t = Target::parse("http://192.168.1.20:8740/?k=abc#x").unwrap();
        assert_eq!(t, Target { host: "192.168.1.20".into(), port: 8740, token: "abc".into() });
        assert_eq!(t.url(), "http://192.168.1.20:8740/?k=abc");
        let t = Target::parse("[::1]:9000?k=t").unwrap();
        assert_eq!((t.host.as_str(), t.port), ("::1", 9000));
        assert!(Target::parse("https://x.trycloudflare.com/?k=a").is_err());
        assert!(Target::parse("http://:80").is_err());
    }
}
