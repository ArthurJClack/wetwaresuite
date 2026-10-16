//! achclwss-share: share a folder with a browser, live-sync folders between
//! machines, and send one-off files, over the LAN or the internet. Standard
//! library only.

pub mod client;
pub mod discovery;
pub mod http;
pub mod ignore;
pub mod index;
pub mod internet;
pub mod json;
pub mod mcp;
pub mod paths;
pub mod qr;
pub mod rand;
pub mod server;
pub mod sha256;
pub mod sync;
pub mod util;
pub mod zip;

pub const VERSION: &str = env!("CARGO_PKG_VERSION");
/// Default TCP port for shares; the next free one is used if it is taken.
pub const DEFAULT_PORT: u16 = 8740;
/// UDP port for LAN announcements.
pub const DISCOVERY_PORT: u16 = 8741;
