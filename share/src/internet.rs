//! `--internet`: there is no wetwaresuite relay, so we use what the machine has.
//! 1. cloudflared on PATH: start a quick tunnel and use its https URL
//! 2. tailscale installed and up: use this machine's tailnet address
//! 3. otherwise: explain the options in one line
//!
//! The share token is always required, whichever path is used.

use std::io::{BufRead, BufReader};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::time::Duration;

pub enum Exposure {
    /// A cloudflared quick tunnel; the child must stay alive.
    Cloudflared { url: String, child: Child },
    Tailscale { url: String },
    None { advice: String },
}

impl Exposure {
    pub fn url(&self) -> Option<&str> {
        match self {
            Exposure::Cloudflared { url, .. } | Exposure::Tailscale { url } => Some(url),
            Exposure::None { .. } => None,
        }
    }

    pub fn describe(&self) -> String {
        match self {
            Exposure::Cloudflared { .. } => "cloudflared quick tunnel (https)".into(),
            Exposure::Tailscale { .. } => "tailscale (encrypted tailnet, only your devices)".into(),
            Exposure::None { advice } => advice.clone(),
        }
    }

    pub fn stop(&mut self) {
        if let Exposure::Cloudflared { child, .. } = self {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

impl Drop for Exposure {
    fn drop(&mut self) {
        self.stop();
    }
}

fn on_path(program: &str) -> bool {
    let exe = if cfg!(windows) { format!("{program}.exe") } else { program.to_string() };
    std::env::var_os("PATH")
        .map(|paths| std::env::split_paths(&paths).any(|d| d.join(&exe).is_file()))
        .unwrap_or(false)
}

/// Finds the trycloudflare URL in a cloudflared log line.
pub fn find_tunnel_url(line: &str) -> Option<String> {
    let start = line.find("https://")?;
    let url: String = line[start..].chars().take_while(|c| !c.is_whitespace() && *c != '|' && *c != '"').collect();
    if url.contains(".trycloudflare.com") || url.contains(".cfargotunnel.com") {
        Some(url.trim_end_matches('/').to_string())
    } else {
        None
    }
}

fn cloudflared(port: u16, token: &str) -> Result<Exposure, String> {
    let mut child = Command::new("cloudflared")
        .args(["tunnel", "--no-autoupdate", "--url", &format!("http://127.0.0.1:{port}")])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("couldn't start cloudflared: {e}"))?;
    let stderr = child.stderr.take().ok_or("no cloudflared output")?;
    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || {
        let mut sent = false;
        for line in BufReader::new(stderr).lines() {
            let Ok(line) = line else { break };
            if !sent {
                if let Some(u) = find_tunnel_url(&line) {
                    let _ = tx.send(u);
                    sent = true;
                }
            }
            // keep draining so cloudflared never blocks on a full pipe
        }
    });
    match rx.recv_timeout(Duration::from_secs(30)) {
        Ok(url) => Ok(Exposure::Cloudflared { url: format!("{url}/?k={token}"), child }),
        Err(_) => {
            let _ = child.kill();
            Err("cloudflared started but reported no tunnel URL within 30 s".into())
        }
    }
}

fn tailscale(port: u16, token: &str) -> Option<Exposure> {
    let out = Command::new("tailscale").args(["ip", "-4"]).stdin(Stdio::null()).output().ok()?;
    if !out.status.success() {
        return None;
    }
    let ip = String::from_utf8_lossy(&out.stdout).lines().next()?.trim().to_string();
    ip.parse::<std::net::Ipv4Addr>().ok()?;
    Some(Exposure::Tailscale { url: format!("http://{ip}:{port}/?k={token}") })
}

pub fn advice(port: u16) -> String {
    format!(
        "no internet route found: install cloudflared (free quick tunnel, https) or tailscale (private tailnet), \
         or forward TCP port {port} on your router and share http://<public-ip>:{port}/?k=<token>"
    )
}

/// Picks the best way to reach the share from outside the LAN. `local_only`
/// means the server listens on 127.0.0.1, which a tunnel can still reach but
/// a tailnet peer cannot.
pub fn expose(port: u16, token: &str, local_only: bool) -> Exposure {
    let mut notes = Vec::new();
    if on_path("cloudflared") {
        match cloudflared(port, token) {
            Ok(e) => return e,
            Err(e) => notes.push(e),
        }
    }
    if !local_only && on_path("tailscale") {
        if let Some(e) = tailscale(port, token) {
            return e;
        }
        notes.push("tailscale is installed but not up (run `tailscale up`)".into());
    }
    notes.push(advice(port));
    Exposure::None { advice: notes.join("; ") }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_quick_tunnel_url() {
        let line = "2024-01-01T00:00:00Z INF |  https://brave-lion-quick.trycloudflare.com                    |";
        assert_eq!(find_tunnel_url(line).unwrap(), "https://brave-lion-quick.trycloudflare.com");
        assert!(find_tunnel_url("INF see https://developers.cloudflare.com/docs").is_none());
        assert!(find_tunnel_url("nothing here").is_none());
    }
}
