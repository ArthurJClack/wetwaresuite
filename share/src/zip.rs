//! A streaming ZIP writer with stored (uncompressed) entries, for "download
//! this folder". Entries are written as they are read, with the CRC and sizes
//! in a data descriptor afterwards, so nothing is buffered and the download
//! starts at once. ZIP64 records are added when an entry or the archive
//! passes 4 GB.

use std::io::{self, Read, Write};

pub fn crc32_table() -> &'static [u32; 256] {
    static TABLE: std::sync::OnceLock<[u32; 256]> = std::sync::OnceLock::new();
    TABLE.get_or_init(|| {
        let mut t = [0u32; 256];
        for (i, slot) in t.iter_mut().enumerate() {
            let mut c = i as u32;
            for _ in 0..8 {
                c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            }
            *slot = c;
        }
        t
    })
}

pub fn crc32_update(crc: u32, data: &[u8]) -> u32 {
    let t = crc32_table();
    let mut c = !crc;
    for b in data {
        c = t[((c ^ *b as u32) & 0xFF) as usize] ^ (c >> 8);
    }
    !c
}

struct Entry {
    name: String,
    crc: u32,
    size: u64,
    offset: u64,
    dos_time: u16,
    dos_date: u16,
    zip64: bool,
}

pub struct ZipWriter<W: Write> {
    out: W,
    offset: u64,
    entries: Vec<Entry>,
    /// Use ZIP64 records for every entry (tests use this to exercise them).
    pub force_zip64: bool,
}

const LIMIT: u64 = 0xFFFF_FFFF;

/// MS-DOS date and time for a unix time (UTC; zip has no time zones).
pub fn dos_datetime(unix_secs: u64) -> (u16, u16) {
    let (y, mo, d, h, mi, s) = crate::util::utc_parts(unix_secs);
    if y < 1980 {
        return (0, (1 << 5) | 1);
    }
    let time = ((h as u16) << 11) | ((mi as u16) << 5) | ((s / 2) as u16);
    let date = (((y - 1980).min(127) as u16) << 9) | ((mo as u16) << 5) | d as u16;
    (time, date)
}

impl<W: Write> ZipWriter<W> {
    pub fn new(out: W) -> Self {
        ZipWriter { out, offset: 0, entries: Vec::new(), force_zip64: false }
    }

    fn put(&mut self, bytes: &[u8]) -> io::Result<()> {
        self.out.write_all(bytes)?;
        self.offset += bytes.len() as u64;
        Ok(())
    }

    /// Adds a folder entry (`name` without the trailing slash).
    pub fn add_dir(&mut self, name: &str, mtime: u64) -> io::Result<()> {
        self.add_file(&format!("{name}/"), mtime, 0, &mut io::empty()).map(|_| ())
    }

    /// Streams one file. `size_hint` is the expected size (from metadata) and
    /// only decides whether ZIP64 fields are needed; the real size is counted.
    pub fn add_file(&mut self, name: &str, mtime: u64, size_hint: u64, data: &mut dyn Read) -> io::Result<u64> {
        let (dos_time, dos_date) = dos_datetime(mtime);
        let zip64 = self.force_zip64 || size_hint >= LIMIT / 2 || self.offset >= LIMIT;
        let offset = self.offset;
        let mut h = Vec::with_capacity(30 + name.len() + 20);
        h.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
        h.extend_from_slice(&(if zip64 { 45u16 } else { 20u16 }).to_le_bytes()); // version needed
        h.extend_from_slice(&0x0808u16.to_le_bytes()); // data descriptor + UTF-8 names
        h.extend_from_slice(&0u16.to_le_bytes()); // stored
        h.extend_from_slice(&dos_time.to_le_bytes());
        h.extend_from_slice(&dos_date.to_le_bytes());
        h.extend_from_slice(&0u32.to_le_bytes()); // crc, in the descriptor
        let placeholder = if zip64 { LIMIT as u32 } else { 0 };
        h.extend_from_slice(&placeholder.to_le_bytes());
        h.extend_from_slice(&placeholder.to_le_bytes());
        h.extend_from_slice(&(name.len() as u16).to_le_bytes());
        h.extend_from_slice(&(if zip64 { 20u16 } else { 0u16 }).to_le_bytes());
        h.extend_from_slice(name.as_bytes());
        if zip64 {
            h.extend_from_slice(&0x0001u16.to_le_bytes());
            h.extend_from_slice(&16u16.to_le_bytes());
            h.extend_from_slice(&0u64.to_le_bytes());
            h.extend_from_slice(&0u64.to_le_bytes());
        }
        self.put(&h)?;

        let mut crc = 0u32;
        let mut size = 0u64;
        let mut buf = vec![0u8; 128 * 1024];
        loop {
            let n = data.read(&mut buf)?;
            if n == 0 {
                break;
            }
            crc = crc32_update(crc, &buf[..n]);
            size += n as u64;
            self.out.write_all(&buf[..n])?;
            self.offset += n as u64;
        }
        if !zip64 && size >= LIMIT {
            return Err(io::Error::other("file grew past 4 GB while zipping"));
        }

        let mut d = Vec::with_capacity(24);
        d.extend_from_slice(&0x0807_4b50u32.to_le_bytes());
        d.extend_from_slice(&crc.to_le_bytes());
        if zip64 {
            d.extend_from_slice(&size.to_le_bytes());
            d.extend_from_slice(&size.to_le_bytes());
        } else {
            d.extend_from_slice(&(size as u32).to_le_bytes());
            d.extend_from_slice(&(size as u32).to_le_bytes());
        }
        self.put(&d)?;
        self.entries.push(Entry { name: name.to_string(), crc, size, offset, dos_time, dos_date, zip64 });
        Ok(size)
    }

    /// Writes the central directory and returns the inner writer.
    pub fn finish(mut self) -> io::Result<W> {
        let cd_start = self.offset;
        let entries = std::mem::take(&mut self.entries);
        for e in &entries {
            let big = e.zip64 || e.size >= LIMIT || e.offset >= LIMIT;
            let mut c = Vec::with_capacity(46 + e.name.len() + 28);
            c.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
            c.extend_from_slice(&(if big { 45u16 } else { 20u16 } | (3 << 8)).to_le_bytes()); // made by: unix
            c.extend_from_slice(&(if big { 45u16 } else { 20u16 }).to_le_bytes());
            c.extend_from_slice(&0x0808u16.to_le_bytes());
            c.extend_from_slice(&0u16.to_le_bytes());
            c.extend_from_slice(&e.dos_time.to_le_bytes());
            c.extend_from_slice(&e.dos_date.to_le_bytes());
            c.extend_from_slice(&e.crc.to_le_bytes());
            let (sz, off) = if big { (LIMIT as u32, LIMIT as u32) } else { (e.size as u32, e.offset as u32) };
            c.extend_from_slice(&sz.to_le_bytes());
            c.extend_from_slice(&sz.to_le_bytes());
            c.extend_from_slice(&(e.name.len() as u16).to_le_bytes());
            c.extend_from_slice(&(if big { 28u16 } else { 0u16 }).to_le_bytes());
            c.extend_from_slice(&0u16.to_le_bytes()); // comment
            c.extend_from_slice(&0u16.to_le_bytes()); // disk
            c.extend_from_slice(&0u16.to_le_bytes()); // internal attrs
            let mode: u32 = if e.name.ends_with('/') { 0o040755 } else { 0o100644 };
            let dos_attr: u32 = if e.name.ends_with('/') { 0x10 } else { 0 };
            c.extend_from_slice(&((mode << 16) | dos_attr).to_le_bytes());
            c.extend_from_slice(&off.to_le_bytes());
            c.extend_from_slice(e.name.as_bytes());
            if big {
                c.extend_from_slice(&0x0001u16.to_le_bytes());
                c.extend_from_slice(&24u16.to_le_bytes());
                c.extend_from_slice(&e.size.to_le_bytes());
                c.extend_from_slice(&e.size.to_le_bytes());
                c.extend_from_slice(&e.offset.to_le_bytes());
            }
            self.put(&c)?;
        }
        let cd_size = self.offset - cd_start;
        let count = entries.len() as u64;
        let need64 = self.force_zip64 || count >= 0xFFFF || cd_start >= LIMIT || cd_size >= LIMIT;
        if need64 {
            let eocd64 = self.offset;
            let mut z = Vec::with_capacity(56 + 20);
            z.extend_from_slice(&0x0606_4b50u32.to_le_bytes());
            z.extend_from_slice(&44u64.to_le_bytes());
            z.extend_from_slice(&45u16.to_le_bytes());
            z.extend_from_slice(&45u16.to_le_bytes());
            z.extend_from_slice(&0u32.to_le_bytes());
            z.extend_from_slice(&0u32.to_le_bytes());
            z.extend_from_slice(&count.to_le_bytes());
            z.extend_from_slice(&count.to_le_bytes());
            z.extend_from_slice(&cd_size.to_le_bytes());
            z.extend_from_slice(&cd_start.to_le_bytes());
            z.extend_from_slice(&0x0706_4b50u32.to_le_bytes()); // locator
            z.extend_from_slice(&0u32.to_le_bytes());
            z.extend_from_slice(&eocd64.to_le_bytes());
            z.extend_from_slice(&1u32.to_le_bytes());
            self.put(&z)?;
        }
        let mut e = Vec::with_capacity(22);
        e.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
        e.extend_from_slice(&0u16.to_le_bytes());
        e.extend_from_slice(&0u16.to_le_bytes());
        let n16 = if need64 { 0xFFFF } else { count as u16 };
        e.extend_from_slice(&n16.to_le_bytes());
        e.extend_from_slice(&n16.to_le_bytes());
        e.extend_from_slice(&(if need64 { LIMIT as u32 } else { cd_size as u32 }).to_le_bytes());
        e.extend_from_slice(&(if need64 { LIMIT as u32 } else { cd_start as u32 }).to_le_bytes());
        e.extend_from_slice(&0u16.to_le_bytes());
        self.put(&e)?;
        self.out.flush()?;
        Ok(self.out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_known_value() {
        assert_eq!(crc32_update(0, b"123456789"), 0xCBF4_3926);
        assert_eq!(crc32_update(crc32_update(0, b"1234"), b"56789"), 0xCBF4_3926);
    }

    #[test]
    fn dos_times() {
        // 2024-03-05 14:30:22 UTC
        let (t, d) = dos_datetime(1_709_649_022);
        assert_eq!(d, (44 << 9) | (3 << 5) | 5);
        assert_eq!(t, (14 << 11) | (30 << 5) | 11);
    }

    /// Parses our own output back through the central directory, the way an
    /// unzip tool does, and checks every entry's data and CRC.
    fn read_back(bytes: &[u8]) -> Vec<(String, Vec<u8>)> {
        let eocd = bytes.len() - 22;
        assert_eq!(&bytes[eocd..eocd + 4], &0x0605_4b50u32.to_le_bytes());
        let le16 = |o: usize| u16::from_le_bytes([bytes[o], bytes[o + 1]]) as u64;
        let le32 = |o: usize| u32::from_le_bytes(bytes[o..o + 4].try_into().unwrap()) as u64;
        let le64 = |o: usize| u64::from_le_bytes(bytes[o..o + 8].try_into().unwrap());
        let mut count = le16(eocd + 10);
        let mut cd = le32(eocd + 16);
        if count == 0xFFFF {
            let loc = eocd - 20;
            assert_eq!(le32(loc), 0x0706_4b50);
            let z = le64(loc + 8) as usize;
            assert_eq!(le32(z), 0x0606_4b50);
            count = le64(z + 32);
            cd = le64(z + 48);
        }
        let mut out = Vec::new();
        let mut p = cd as usize;
        for _ in 0..count {
            assert_eq!(le32(p), 0x0201_4b50);
            let crc = le32(p + 16) as u32;
            let mut size = le32(p + 24);
            let name_len = le16(p + 28) as usize;
            let extra_len = le16(p + 30) as usize;
            let mut off = le32(p + 42);
            let name = String::from_utf8(bytes[p + 46..p + 46 + name_len].to_vec()).unwrap();
            if extra_len > 0 {
                let x = p + 46 + name_len;
                assert_eq!(le16(x), 1);
                size = le64(x + 4);
                off = le64(x + 20);
            }
            let lh = off as usize;
            assert_eq!(le32(lh), 0x0403_4b50);
            let data_start = lh + 30 + le16(lh + 26) as usize + le16(lh + 28) as usize;
            let data = bytes[data_start..data_start + size as usize].to_vec();
            assert_eq!(crc32_update(0, &data), crc);
            out.push((name, data));
            p += 46 + name_len + extra_len;
        }
        out
    }

    #[test]
    fn writes_readable_archive() {
        for force in [false, true] {
            let mut z = ZipWriter::new(Vec::new());
            z.force_zip64 = force;
            z.add_dir("docs", 1_700_000_000).unwrap();
            z.add_file("docs/a.txt", 1_700_000_000, 5, &mut &b"hello"[..]).unwrap();
            z.add_file("ünï/empty", 1_700_000_000, 0, &mut io::empty()).unwrap();
            let big: Vec<u8> = (0..300_000u32).map(|i| (i % 251) as u8).collect();
            z.add_file("big.bin", 1_700_000_000, big.len() as u64, &mut &big[..]).unwrap();
            let bytes = z.finish().unwrap();
            let entries = read_back(&bytes);
            let names: Vec<&str> = entries.iter().map(|(n, _)| n.as_str()).collect();
            assert_eq!(names, vec!["docs/", "docs/a.txt", "ünï/empty", "big.bin"]);
            assert_eq!(entries[1].1, b"hello");
            assert_eq!(entries[3].1, big);
        }
    }
}
