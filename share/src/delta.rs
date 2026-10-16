//! rsync-style block deltas, so a small edit to a big file sends only the
//! changed bytes.
//!
//! The side that already has an old copy (the receiver) cuts it into blocks
//! and sends a [`Signature`]: a weak rolling checksum (Adler-32 style) and a
//! SHA-256 per block. The side with the new copy (the sender) slides a window
//! over its file one byte at a time; wherever the rolling checksum and then the
//! SHA-256 match a block, it sends "copy block N" instead of the bytes. The
//! rest goes as literal bytes. The receiver rebuilds the file into a temporary
//! file and checks the whole-file SHA-256 carried in the delta before the
//! caller renames it into place, so a bad delta can never land; the caller
//! then falls back to sending the whole file.
//!
//! Wire formats (all integers little-endian):
//! - signature: `WSG1`, block size u32, basis length u64, block count u32,
//!   then per block: weak u32, strong [u8; 32]
//! - delta: `WDL1`, block size u32, target length u64, target SHA-256 [u8; 32],
//!   then ops: `1` copy (first block u32, count u32), `2` literal (len u32,
//!   bytes), `0` end

use std::collections::HashMap;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::sha256::{self, Sha256};

/// Files smaller than this are always sent whole: a delta can't save much.
pub const MIN_SIZE: u64 = 64 * 1024;
/// Files bigger than this are sent whole: deltas are built in memory.
pub const MAX_SIZE: u64 = 512 * 1024 * 1024;
/// Largest signature a host accepts (a 512 MB file at 16 KB blocks is ~1.2 MB).
pub const MAX_SIGNATURE: u64 = 16 * 1024 * 1024;

const SIG_MAGIC: &[u8; 4] = b"WSG1";
const DELTA_MAGIC: &[u8; 4] = b"WDL1";
const DELTA_HEAD: usize = 4 + 4 + 8 + 32;
const OP_END: u8 = 0;
const OP_COPY: u8 = 1;
const OP_LITERAL: u8 = 2;

/// Test hook: when set, the sync engine damages every delta it builds or
/// receives, to prove the whole-file fallback works.
#[doc(hidden)]
pub static CORRUPT_FOR_TEST: AtomicBool = AtomicBool::new(false);

/// Flips a byte of the target hash when the test hook is on.
pub fn maybe_corrupt(delta: &mut [u8]) {
    if CORRUPT_FOR_TEST.load(Ordering::Relaxed) && delta.len() >= DELTA_HEAD {
        delta[DELTA_HEAD - 1] ^= 0x5a;
    }
}

/// About sqrt(file size), clamped to 2 KB..64 KB.
pub fn block_size(len: u64) -> u32 {
    ((len as f64).sqrt() as u64).clamp(2048, 65536) as u32
}

fn bad(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg.to_string())
}

/// The Adler-32 style weak checksum rsync uses, which can roll by one byte.
#[derive(Clone, Copy, Default)]
struct Rolling {
    a: u32,
    b: u32,
    len: u32,
}

impl Rolling {
    fn new(data: &[u8]) -> Rolling {
        let mut r = Rolling { a: 0, b: 0, len: data.len() as u32 };
        for (i, &x) in data.iter().enumerate() {
            r.a = r.a.wrapping_add(x as u32);
            r.b = r.b.wrapping_add((data.len() - i) as u32 * x as u32);
        }
        r
    }

    fn value(&self) -> u32 {
        (self.a & 0xffff) | (self.b << 16)
    }

    /// Slides the window one byte: `out` leaves on the left, `inn` joins on the right.
    fn roll(&mut self, out: u8, inn: u8) {
        self.a = self.a.wrapping_sub(out as u32).wrapping_add(inn as u32);
        self.b = self.b.wrapping_sub(self.len.wrapping_mul(out as u32)).wrapping_add(self.a);
    }
}

fn weak(data: &[u8]) -> u32 {
    Rolling::new(data).value()
}

fn strong(data: &[u8]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(data);
    h.finish()
}

/// Block checksums of the receiver's copy.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Signature {
    pub block: u32,
    pub len: u64,
    pub blocks: Vec<(u32, [u8; 32])>,
}

impl Signature {
    pub fn of_reader(r: &mut dyn Read, len: u64) -> io::Result<Signature> {
        let block = block_size(len);
        let mut buf = vec![0u8; block as usize];
        let mut blocks = Vec::with_capacity(len.div_ceil(block as u64) as usize);
        let mut left = len;
        while left > 0 {
            let n = left.min(block as u64) as usize;
            r.read_exact(&mut buf[..n])?;
            blocks.push((weak(&buf[..n]), strong(&buf[..n])));
            left -= n as u64;
        }
        Ok(Signature { block, len, blocks })
    }

    pub fn of_file(path: &Path) -> io::Result<Signature> {
        let f = std::fs::File::open(path)?;
        let len = f.metadata()?.len();
        Signature::of_reader(&mut io::BufReader::with_capacity(256 * 1024, f), len)
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(20 + self.blocks.len() * 36);
        out.extend_from_slice(SIG_MAGIC);
        out.extend_from_slice(&self.block.to_le_bytes());
        out.extend_from_slice(&self.len.to_le_bytes());
        out.extend_from_slice(&(self.blocks.len() as u32).to_le_bytes());
        for (w, s) in &self.blocks {
            out.extend_from_slice(&w.to_le_bytes());
            out.extend_from_slice(s);
        }
        out
    }

    pub fn decode(b: &[u8]) -> io::Result<Signature> {
        if b.len() < 20 || &b[..4] != SIG_MAGIC {
            return Err(bad("not a block signature"));
        }
        let block = u32::from_le_bytes(b[4..8].try_into().unwrap());
        let len = u64::from_le_bytes(b[8..16].try_into().unwrap());
        let count = u32::from_le_bytes(b[16..20].try_into().unwrap()) as u64;
        if !(1..=1 << 20).contains(&block) || len.div_ceil(block as u64) != count || b.len() as u64 != 20 + count * 36 {
            return Err(bad("damaged block signature"));
        }
        let blocks = b[20..]
            .chunks_exact(36)
            .map(|c| (u32::from_le_bytes(c[..4].try_into().unwrap()), c[4..].try_into().unwrap()))
            .collect();
        Ok(Signature { block, len, blocks })
    }
}

struct Writer {
    out: Vec<u8>,
    run: Option<(u32, u32)>,
}

impl Writer {
    fn flush_run(&mut self) {
        if let Some((first, count)) = self.run.take() {
            self.out.push(OP_COPY);
            self.out.extend_from_slice(&first.to_le_bytes());
            self.out.extend_from_slice(&count.to_le_bytes());
        }
    }

    fn copy(&mut self, idx: u32) {
        match &mut self.run {
            Some((first, count)) if *first + *count == idx => *count += 1,
            _ => {
                self.flush_run();
                self.run = Some((idx, 1));
            }
        }
    }

    fn literal(&mut self, data: &[u8]) {
        if data.is_empty() {
            return;
        }
        self.flush_run();
        for chunk in data.chunks(1 << 30) {
            self.out.push(OP_LITERAL);
            self.out.extend_from_slice(&(chunk.len() as u32).to_le_bytes());
            self.out.extend_from_slice(chunk);
        }
    }
}

/// Builds the delta that turns the signed file into `data`.
pub fn delta(sig: &Signature, data: &[u8]) -> Vec<u8> {
    let bs = sig.block as usize;
    let mut w = Writer { out: Vec::with_capacity(DELTA_HEAD + 1024), run: None };
    w.out.extend_from_slice(DELTA_MAGIC);
    w.out.extend_from_slice(&sig.block.to_le_bytes());
    w.out.extend_from_slice(&(data.len() as u64).to_le_bytes());
    w.out.extend_from_slice(&strong(data));

    // full-size blocks by weak checksum; a short last block is checked only at the tail
    let mut table: HashMap<u32, Vec<u32>> = HashMap::new();
    let mut tail_block = None;
    for (i, (wk, _)) in sig.blocks.iter().enumerate() {
        let full = (i as u64 + 1) * bs as u64 <= sig.len;
        if full {
            table.entry(*wk).or_default().push(i as u32);
        } else {
            tail_block = Some((i as u32, (sig.len - i as u64 * bs as u64) as usize));
        }
    }
    let find = |window: &[u8], wk: u32| -> Option<u32> {
        let cands = table.get(&wk)?;
        let s = strong(window);
        cands.iter().copied().find(|&j| sig.blocks[j as usize].1 == s)
    };

    let n = data.len();
    let mut lit = 0;
    let mut i = 0;
    let mut roll = if n >= bs && !table.is_empty() { Some(Rolling::new(&data[..bs])) } else { None };
    while let Some(r) = roll.as_mut() {
        if i + bs > n {
            break;
        }
        if let Some(idx) = find(&data[i..i + bs], r.value()) {
            w.literal(&data[lit..i]);
            w.copy(idx);
            i += bs;
            lit = i;
            if i + bs <= n {
                *r = Rolling::new(&data[i..i + bs]);
            }
            continue;
        }
        if i + bs < n {
            r.roll(data[i], data[i + bs]);
        }
        i += 1;
    }
    if let Some((idx, len)) = tail_block {
        if n >= lit + len {
            let start = n - len;
            let window = &data[start..];
            if weak(window) == sig.blocks[idx as usize].0 && strong(window) == sig.blocks[idx as usize].1 {
                w.literal(&data[lit..start]);
                w.copy(idx);
                lit = n;
            }
        }
    }
    w.literal(&data[lit..]);
    w.flush_run();
    w.out.push(OP_END);
    w.out
}

/// Rebuilds the new file from the old one (`basis`) and a delta into `out`.
/// Returns the new file's SHA-256 hex; any damage or mismatch is an
/// `InvalidData` error, in which case `out` must be thrown away.
pub fn apply<B: Read + Seek>(basis: &mut B, d: &[u8], out: &mut dyn Write) -> io::Result<String> {
    if d.len() < DELTA_HEAD + 1 || &d[..4] != DELTA_MAGIC {
        return Err(bad("not a delta"));
    }
    let bs = u32::from_le_bytes(d[4..8].try_into().unwrap()) as u64;
    let target_len = u64::from_le_bytes(d[8..16].try_into().unwrap());
    let want: [u8; 32] = d[16..48].try_into().unwrap();
    if bs == 0 {
        return Err(bad("damaged delta"));
    }
    let basis_len = basis.seek(SeekFrom::End(0))?;
    let mut h = Sha256::new();
    let mut written = 0u64;
    let mut buf = vec![0u8; 256 * 1024];
    let mut p = DELTA_HEAD;
    let take = |p: &mut usize, n: usize| -> io::Result<&[u8]> {
        let s = d.get(*p..*p + n).ok_or_else(|| bad("delta is cut short"))?;
        *p += n;
        Ok(s)
    };
    loop {
        let op = take(&mut p, 1)?[0];
        match op {
            OP_END => break,
            OP_COPY => {
                let first = u32::from_le_bytes(take(&mut p, 4)?.try_into().unwrap()) as u64;
                let count = u32::from_le_bytes(take(&mut p, 4)?.try_into().unwrap()) as u64;
                let off = first * bs;
                if count == 0 || off >= basis_len {
                    return Err(bad("delta copies past the end of the old file"));
                }
                let end = (off + count * bs).min(basis_len);
                if off + count * bs - end >= bs {
                    return Err(bad("delta copies past the end of the old file"));
                }
                basis.seek(SeekFrom::Start(off))?;
                let mut left = end - off;
                while left > 0 {
                    let n = left.min(buf.len() as u64) as usize;
                    basis.read_exact(&mut buf[..n])?;
                    h.update(&buf[..n]);
                    out.write_all(&buf[..n])?;
                    left -= n as u64;
                }
                written += end - off;
            }
            OP_LITERAL => {
                let len = u32::from_le_bytes(take(&mut p, 4)?.try_into().unwrap()) as usize;
                let bytes = take(&mut p, len)?;
                h.update(bytes);
                out.write_all(bytes)?;
                written += len as u64;
            }
            _ => return Err(bad("damaged delta")),
        }
        if written > target_len {
            return Err(bad("delta builds a longer file than it says"));
        }
    }
    if p != d.len() || written != target_len {
        return Err(bad("delta builds the wrong length"));
    }
    let got = h.finish();
    if got != want {
        return Err(bad("rebuilt file does not match its checksum"));
    }
    Ok(sha256::hex(&got))
}

/// [`apply`] from a basis file into `tmp`; removes `tmp` on any error.
pub fn apply_to_file(basis: &Path, d: &[u8], tmp: &Path) -> io::Result<String> {
    let result = (|| {
        let mut b = std::fs::File::open(basis)?;
        let mut f = io::BufWriter::with_capacity(256 * 1024, std::fs::File::create(tmp)?);
        let hash = apply(&mut b, d, &mut f)?;
        f.flush()?;
        Ok(hash)
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(tmp);
    }
    result
}

/// The target length and SHA-256 hex a delta claims to build.
pub fn target_of(d: &[u8]) -> Option<(u64, String)> {
    if d.len() < DELTA_HEAD || &d[..4] != DELTA_MAGIC {
        return None;
    }
    Some((u64::from_le_bytes(d[8..16].try_into().unwrap()), sha256::hex(&d[16..48])))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

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

    fn roundtrip(old: &[u8], new: &[u8]) -> usize {
        let sig = Signature::of_reader(&mut Cursor::new(old), old.len() as u64).unwrap();
        let sig = Signature::decode(&sig.encode()).unwrap();
        let d = delta(&sig, new);
        let mut out = Vec::new();
        let hash = apply(&mut Cursor::new(old), &d, &mut out).unwrap();
        assert!(out == new, "rebuilt file differs");
        assert_eq!(hash, sha256::digest_hex(new));
        d.len()
    }

    #[test]
    fn rolling_matches_fresh() {
        let data = random(5000, 3);
        let mut r = Rolling::new(&data[..2048]);
        for i in 0..1000 {
            r.roll(data[i], data[i + 2048]);
            assert_eq!(r.value(), weak(&data[i + 1..i + 2049]), "at {i}");
        }
    }

    #[test]
    fn block_sizes() {
        assert_eq!(block_size(0), 2048);
        assert_eq!(block_size(5 << 20), 2289);
        assert_eq!(block_size(100 << 30), 65536);
    }

    #[test]
    fn edits_in_a_big_file_send_little() {
        let old = random(5 << 20, 42);
        // insert in the middle
        let mut ins = old.clone();
        ins.splice(2_500_000..2_500_000, random(1000, 7));
        let n = roundtrip(&old, &ins);
        assert!(n < 40_000, "insert sent {n} bytes");
        // delete in the middle
        let mut del = old.clone();
        del.drain(1_000_000..1_003_333);
        let n = roundtrip(&old, &del);
        assert!(n < 40_000, "delete sent {n} bytes");
        // append
        let mut app = old.clone();
        app.extend_from_slice(&random(5000, 9));
        let n = roundtrip(&old, &app);
        assert!(n < 40_000, "append sent {n} bytes");
        // overwrite a few bytes in several places
        let mut ed = old.clone();
        for p in [10usize, 777_777, 4_000_000, ed.len() - 1] {
            ed[p] ^= 0xff;
        }
        let n = roundtrip(&old, &ed);
        assert!(n < 40_000, "edits sent {n} bytes");
        // identical
        assert!(roundtrip(&old, &old) < 200);
    }

    #[test]
    fn edge_cases_round_trip() {
        roundtrip(b"", b"");
        roundtrip(b"", b"new file");
        roundtrip(b"old file", b"");
        roundtrip(&random(3000, 1), &random(3000, 1)); // one full + one short block
        let old = random(10_000, 5);
        roundtrip(&old, &old[..9_000]); // truncate
        roundtrip(&old, &[&old[..], &old[..]].concat()); // repeat
        roundtrip(&old, &random(10_000, 6)); // unrelated
    }

    #[test]
    fn damage_is_caught() {
        let old = random(200_000, 11);
        let mut new = old.clone();
        new[100_000] ^= 1;
        let sig = Signature::of_reader(&mut Cursor::new(&old), old.len() as u64).unwrap();
        let d = delta(&sig, &new);
        // flip each region of the delta: header hash, a copy index, a literal byte, the end
        let mut targets = vec![DELTA_HEAD - 1, DELTA_HEAD + 2, d.len() / 2, d.len() - 1];
        targets.push(d.len() - 2);
        for t in targets {
            let mut bad = d.clone();
            bad[t] ^= 0x41;
            let mut out = Vec::new();
            assert!(apply(&mut Cursor::new(&old), &bad, &mut out).is_err(), "damage at {t} slipped through");
        }
        let mut out = Vec::new();
        assert!(apply(&mut Cursor::new(&old), &d[..d.len() - 3], &mut out).is_err());
        // the right delta against a different basis fails too
        let mut other = old.clone();
        other[5] ^= 1;
        assert!(apply(&mut Cursor::new(&other), &d, &mut Vec::new()).is_err());
        assert!(Signature::decode(&sig.encode()[..30]).is_err());
    }
}
