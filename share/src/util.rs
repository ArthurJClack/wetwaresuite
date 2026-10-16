//! Small helpers: host name, LAN addresses, URL encoding, sizes and times.

use std::net::{IpAddr, Ipv4Addr, UdpSocket};
use std::time::{SystemTime, UNIX_EPOCH};

/// This machine's name, used in announcements and conflict file names.
pub fn hostname() -> String {
    let raw = std::env::var("COMPUTERNAME")
        .ok()
        .or_else(|| std::fs::read_to_string("/proc/sys/kernel/hostname").ok())
        .or_else(|| std::fs::read_to_string("/etc/hostname").ok())
        .or_else(|| std::env::var("HOSTNAME").ok())
        .or_else(|| {
            std::process::Command::new("hostname")
                .output()
                .ok()
                .filter(|o| o.status.success())
                .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        })
        .unwrap_or_default();
    let name = raw.trim().split('.').next().unwrap_or("").to_string();
    let safe: String = name
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' { c } else { '-' })
        .collect();
    if safe.is_empty() {
        "host".into()
    } else {
        safe.to_ascii_lowercase()
    }
}

/// The address of the interface that routes to the outside world, found by
/// "connecting" a UDP socket (no packet is sent). Falls back to nothing.
pub fn primary_lan_ip() -> Option<Ipv4Addr> {
    for probe in ["8.8.8.8:80", "192.168.1.1:80", "10.0.0.1:80"] {
        let Ok(sock) = UdpSocket::bind("0.0.0.0:0") else { continue };
        if sock.connect(probe).is_err() {
            continue;
        }
        if let Ok(addr) = sock.local_addr() {
            if let IpAddr::V4(ip) = addr.ip() {
                if !ip.is_unspecified() && !ip.is_loopback() {
                    return Some(ip);
                }
            }
        }
    }
    None
}

/// Decodes `%XX` escapes (a `+` stays a `+`, as in URL paths).
pub fn percent_decode(s: &str) -> Option<String> {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'%' => {
                let h = bytes.get(i + 1..i + 3)?;
                let v = u8::from_str_radix(std::str::from_utf8(h).ok()?, 16).ok()?;
                out.push(v);
                i += 3;
            }
            b => {
                out.push(b);
                i += 1;
            }
        }
    }
    String::from_utf8(out).ok()
}

/// Encodes one path segment or query value.
pub fn percent_encode(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        if b.is_ascii_alphanumeric() || b"-._~".contains(&b) {
            out.push(b as char);
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
}

/// Encodes a relative path, keeping the slashes between segments.
pub fn encode_path(path: &str) -> String {
    path.split('/').map(percent_encode).collect::<Vec<_>>().join("/")
}

/// Parses `a=1&b=2` into pairs (decoded).
pub fn parse_query(q: &str) -> Vec<(String, String)> {
    q.split('&')
        .filter(|p| !p.is_empty())
        .filter_map(|p| {
            let (k, v) = p.split_once('=').unwrap_or((p, ""));
            Some((percent_decode(&k.replace('+', " "))?, percent_decode(&v.replace('+', " "))?))
        })
        .collect()
}

pub fn human_size(n: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    let mut v = n as f64;
    let mut u = 0;
    while v >= 1024.0 && u < UNITS.len() - 1 {
        v /= 1024.0;
        u += 1;
    }
    if u == 0 {
        format!("{n} B")
    } else {
        format!("{v:.1} {}", UNITS[u])
    }
}

pub fn unix_secs(t: SystemTime) -> u64 {
    t.duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

pub fn unix_nanos(t: SystemTime) -> u128 {
    t.duration_since(UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0)
}

/// Days since 1970-01-01 to (year, month, day), proleptic Gregorian.
pub fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

/// (year, month, day, hour, minute, second) in UTC.
pub fn utc_parts(secs: u64) -> (i64, u32, u32, u32, u32, u32) {
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    let (y, m, d) = civil_from_days(days);
    (y, m, d, (rem / 3600) as u32, (rem / 60 % 60) as u32, (rem % 60) as u32)
}

/// Escapes text for HTML.
pub fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;").replace('\'', "&#39;")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn percent_round_trip() {
        let s = "a b/ü?&=%";
        assert_eq!(percent_decode(&percent_encode(s)).unwrap(), s);
        assert_eq!(encode_path("dir one/a+b.txt"), "dir%20one/a%2Bb.txt");
        assert_eq!(percent_decode("%2e%2E").unwrap(), "..");
        assert!(percent_decode("%zz").is_none());
        assert!(percent_decode("%ff").is_none()); // not UTF-8
    }

    #[test]
    fn query_parsing() {
        let q = parse_query("k=abc&path=a%2Fb&flag");
        assert_eq!(q, vec![("k".into(), "abc".into()), ("path".into(), "a/b".into()), ("flag".into(), "".into())]);
    }

    #[test]
    fn dates() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        assert_eq!(utc_parts(951_782_400), (2000, 2, 29, 0, 0, 0));
        assert_eq!(human_size(1536), "1.5 KB");
        assert_eq!(human_size(12), "12 B");
    }

    #[test]
    fn hostname_is_safe() {
        let h = hostname();
        assert!(!h.is_empty());
        assert!(h.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_'));
    }
}
