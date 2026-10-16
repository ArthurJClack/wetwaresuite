//! LAN discovery: a running share broadcasts a small UDP datagram every two
//! seconds; `peers`, `sync CODE` and `receive CODE` listen for them. The
//! announcement holds no secret (no token, no code): joining by code goes
//! through the share's rate-limited pairing endpoint.

use std::net::{IpAddr, Ipv4Addr, SocketAddr, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::json::Json;
use crate::DISCOVERY_PORT;

const MAGIC: &str = "wetwaresuite-SHARE/1 ";

#[derive(Debug, Clone, PartialEq)]
pub struct Announcement {
    /// Random per share, so repeated datagrams collapse into one peer.
    pub id: String,
    pub host: String,
    pub folder: String,
    pub port: u16,
    pub writable: bool,
    /// "share", "sync" (a sync host), "send" or "receive" (a drop box)
    pub kind: String,
}

#[derive(Debug, Clone)]
pub struct Peer {
    pub ann: Announcement,
    pub addr: IpAddr,
}

impl Peer {
    pub fn base_url(&self) -> String {
        format!("http://{}:{}", self.addr, self.ann.port)
    }
}

impl Announcement {
    pub fn encode(&self) -> Vec<u8> {
        let j = Json::obj()
            .with("id", self.id.as_str())
            .with("host", self.host.as_str())
            .with("folder", self.folder.as_str())
            .with("port", self.port)
            .with("writable", self.writable)
            .with("kind", self.kind.as_str());
        format!("{MAGIC}{}", j.dump()).into_bytes()
    }

    pub fn decode(data: &[u8]) -> Option<Announcement> {
        let text = std::str::from_utf8(data).ok()?.strip_prefix(MAGIC)?;
        let j = Json::parse(text).ok()?;
        let port = j.get("port")?.as_u64()?;
        Some(Announcement {
            id: j.get("id")?.as_str()?.chars().take(64).collect(),
            host: j.str_or("host", "?").chars().take(64).collect(),
            folder: j.str_or("folder", "").chars().take(128).collect(),
            port: u16::try_from(port).ok().filter(|p| *p != 0)?,
            writable: j.bool_or("writable", false),
            kind: j.str_or("kind", "share").chars().take(16).collect(),
        })
    }
}

/// Broadcasts `ann` until `stop` is set. Errors (no network, broadcast not
/// allowed) are ignored: discovery is a convenience, the URL always works.
pub fn announce(ann: Announcement, stop: Arc<AtomicBool>) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        let Ok(sock) = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)) else { return };
        let _ = sock.set_broadcast(true);
        let msg = ann.encode();
        let targets = [
            SocketAddr::from((Ipv4Addr::BROADCAST, DISCOVERY_PORT)),
            // also reach listeners on this machine when broadcast is filtered
            SocketAddr::from((Ipv4Addr::LOCALHOST, DISCOVERY_PORT)),
        ];
        while !stop.load(Ordering::Relaxed) {
            for t in targets {
                let _ = sock.send_to(&msg, t);
            }
            if let Some(ip) = crate::util::primary_lan_ip() {
                // directed broadcast for the /24, which some routers pass when 255.255.255.255 is dropped
                let o = ip.octets();
                let _ = sock.send_to(&msg, SocketAddr::from((Ipv4Addr::new(o[0], o[1], o[2], 255), DISCOVERY_PORT)));
            }
            for _ in 0..20 {
                if stop.load(Ordering::Relaxed) {
                    break;
                }
                std::thread::sleep(Duration::from_millis(100));
            }
        }
    })
}

/// Listens for announcements for up to `wait`, returning distinct shares.
/// Returns early once `want` matches (when given).
pub fn listen(wait: Duration, want: Option<&dyn Fn(&Peer) -> bool>) -> std::io::Result<Vec<Peer>> {
    let sock = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, DISCOVERY_PORT)).map_err(|e| {
        std::io::Error::new(
            e.kind(),
            format!("can't listen on UDP port {DISCOVERY_PORT} for LAN shares ({e}); is another `peers` running?"),
        )
    })?;
    let deadline = Instant::now() + wait;
    let mut peers: Vec<Peer> = Vec::new();
    let mut buf = [0u8; 2048];
    loop {
        let now = Instant::now();
        if now >= deadline {
            break;
        }
        sock.set_read_timeout(Some((deadline - now).max(Duration::from_millis(10))))?;
        match sock.recv_from(&mut buf) {
            Ok((n, from)) => {
                if let Some(ann) = Announcement::decode(&buf[..n]) {
                    let mut addr = from.ip();
                    // prefer a LAN address over loopback for the same share
                    if let Some(p) = peers.iter_mut().find(|p| p.ann.id == ann.id) {
                        if p.addr.is_loopback() && !addr.is_loopback() {
                            p.addr = addr;
                        }
                        continue;
                    }
                    if addr.is_unspecified() {
                        addr = IpAddr::V4(Ipv4Addr::LOCALHOST);
                    }
                    let peer = Peer { ann, addr };
                    let hit = want.is_some_and(|f| f(&peer));
                    peers.push(peer);
                    if hit {
                        break;
                    }
                }
            }
            Err(e) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => break,
            Err(e) if e.kind() == std::io::ErrorKind::ConnectionReset => continue, // Windows ICMP noise
            Err(e) => return Err(e),
        }
    }
    Ok(peers)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn announcement_round_trip() {
        let a = Announcement {
            id: "abc".into(),
            host: "laptop".into(),
            folder: "proj".into(),
            port: 8740,
            writable: true,
            kind: "sync".into(),
        };
        assert_eq!(Announcement::decode(&a.encode()), Some(a));
        assert_eq!(Announcement::decode(b"garbage"), None);
        assert_eq!(Announcement::decode(b"wetwaresuite-SHARE/1 {\"id\":\"x\",\"port\":0}"), None);
        let text = String::from_utf8(Announcement::decode(b"wetwaresuite-SHARE/1 {\"id\":\"x\",\"port\":1}").unwrap().encode()).unwrap();
        assert!(!text.contains("token"));
    }
}
