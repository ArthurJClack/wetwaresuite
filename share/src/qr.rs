//! A minimal QR code encoder: byte mode, error correction M (falling back to
//! L), versions 1 to 10 (up to 213 bytes at L), automatic mask choice. Enough
//! for a share URL, rendered in the terminal with half-block characters.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Ecc {
    L,
    M,
}

impl Ecc {
    fn format_bits(self) -> u32 {
        match self {
            Ecc::L => 1,
            Ecc::M => 0,
        }
    }
}

/// (EC codewords per block, [(block count, data codewords per block)])
fn blocks(version: usize, ecc: Ecc) -> (usize, &'static [(usize, usize)]) {
    const L: [(usize, &[(usize, usize)]); 10] = [
        (7, &[(1, 19)]),
        (10, &[(1, 34)]),
        (15, &[(1, 55)]),
        (20, &[(1, 80)]),
        (26, &[(1, 108)]),
        (18, &[(2, 68)]),
        (20, &[(2, 78)]),
        (24, &[(2, 97)]),
        (30, &[(2, 116)]),
        (18, &[(2, 68), (2, 69)]),
    ];
    const M: [(usize, &[(usize, usize)]); 10] = [
        (10, &[(1, 16)]),
        (16, &[(1, 28)]),
        (26, &[(1, 44)]),
        (18, &[(2, 32)]),
        (24, &[(2, 43)]),
        (16, &[(4, 27)]),
        (18, &[(4, 31)]),
        (22, &[(2, 38), (2, 39)]),
        (22, &[(3, 36), (2, 37)]),
        (26, &[(4, 43), (1, 44)]),
    ];
    match ecc {
        Ecc::L => L[version - 1],
        Ecc::M => M[version - 1],
    }
}

fn data_capacity(version: usize, ecc: Ecc) -> usize {
    blocks(version, ecc).1.iter().map(|(n, d)| n * d).sum()
}

fn alignment_positions(version: usize) -> &'static [usize] {
    const POS: [&[usize]; 10] = [
        &[],
        &[6, 18],
        &[6, 22],
        &[6, 26],
        &[6, 30],
        &[6, 34],
        &[6, 22, 38],
        &[6, 24, 42],
        &[6, 26, 46],
        &[6, 28, 50],
    ];
    POS[version - 1]
}

pub const MAX_VERSION: usize = 10;

#[derive(Debug, Clone)]
pub struct QrCode {
    pub version: usize,
    pub ecc: Ecc,
    pub mask: u8,
    pub size: usize,
    modules: Vec<bool>,
    function: Vec<bool>,
}

// ---- GF(256) Reed-Solomon ----

fn gf_mul(mut a: u8, mut b: u8) -> u8 {
    let mut p = 0u8;
    while b != 0 {
        if b & 1 != 0 {
            p ^= a;
        }
        let carry = a & 0x80;
        a <<= 1;
        if carry != 0 {
            a ^= 0x1D;
        }
        b >>= 1;
    }
    p
}

fn rs_generator(degree: usize) -> Vec<u8> {
    // coefficients, highest power first, leading 1 omitted
    let mut g = vec![0u8; degree];
    g[degree - 1] = 1;
    let mut root = 1u8;
    for _ in 0..degree {
        for j in 0..degree {
            g[j] = gf_mul(g[j], root);
            if j + 1 < degree {
                g[j] ^= g[j + 1];
            }
        }
        root = gf_mul(root, 2);
    }
    g
}

fn rs_remainder(data: &[u8], gen: &[u8]) -> Vec<u8> {
    let mut r = vec![0u8; gen.len()];
    for b in data {
        let factor = b ^ r[0];
        r.remove(0);
        r.push(0);
        for (ri, gi) in r.iter_mut().zip(gen) {
            *ri ^= gf_mul(*gi, factor);
        }
    }
    r
}

struct Bits(Vec<bool>);

impl Bits {
    fn push(&mut self, value: u32, count: usize) {
        for i in (0..count).rev() {
            self.0.push((value >> i) & 1 != 0);
        }
    }
}

impl QrCode {
    /// Encodes bytes at the smallest version that fits, preferring ECC M.
    pub fn encode(data: &[u8]) -> Option<QrCode> {
        for ecc in [Ecc::M, Ecc::L] {
            for v in 1..=MAX_VERSION {
                if Self::fits(data.len(), v, ecc) {
                    return Some(Self::encode_with(data, v, ecc, None));
                }
            }
        }
        None
    }

    fn fits(len: usize, version: usize, ecc: Ecc) -> bool {
        let count_bits = if version < 10 { 8 } else { 16 };
        4 + count_bits + len * 8 <= data_capacity(version, ecc) * 8
    }

    /// Encodes with a fixed version and ECC, and a fixed mask or the best one.
    pub fn encode_with(data: &[u8], version: usize, ecc: Ecc, mask: Option<u8>) -> QrCode {
        assert!(Self::fits(data.len(), version, ecc), "data too long for version {version}");
        let cap = data_capacity(version, ecc);
        let mut bits = Bits(Vec::new());
        bits.push(0b0100, 4);
        bits.push(data.len() as u32, if version < 10 { 8 } else { 16 });
        for b in data {
            bits.push(*b as u32, 8);
        }
        let term = (cap * 8 - bits.0.len()).min(4);
        bits.push(0, term);
        while !bits.0.len().is_multiple_of(8) {
            bits.0.push(false);
        }
        let mut codewords: Vec<u8> =
            bits.0.chunks(8).map(|c| c.iter().fold(0u8, |acc, b| (acc << 1) | *b as u8)).collect();
        let mut pad = 0xECu8;
        while codewords.len() < cap {
            codewords.push(pad);
            pad = if pad == 0xEC { 0x11 } else { 0xEC };
        }

        // split into blocks, add EC, interleave
        let (ec_len, groups) = blocks(version, ecc);
        let gen = rs_generator(ec_len);
        let mut data_blocks: Vec<&[u8]> = Vec::new();
        let mut pos = 0;
        for (count, len) in groups {
            for _ in 0..*count {
                data_blocks.push(&codewords[pos..pos + len]);
                pos += len;
            }
        }
        let ec_blocks: Vec<Vec<u8>> = data_blocks.iter().map(|b| rs_remainder(b, &gen)).collect();
        let max_len = data_blocks.iter().map(|b| b.len()).max().unwrap_or(0);
        let mut out = Vec::new();
        for i in 0..max_len {
            for b in &data_blocks {
                if let Some(c) = b.get(i) {
                    out.push(*c);
                }
            }
        }
        for i in 0..ec_len {
            for b in &ec_blocks {
                out.push(b[i]);
            }
        }

        let size = version * 4 + 17;
        let mut qr =
            QrCode { version, ecc, mask: 0, size, modules: vec![false; size * size], function: vec![false; size * size] };
        qr.draw_function_patterns();
        qr.draw_codewords(&out);
        let mask = mask.unwrap_or_else(|| {
            (0..8u8)
                .min_by_key(|m| {
                    let mut t = qr.clone();
                    t.apply_mask(*m);
                    t.draw_format(*m);
                    t.penalty()
                })
                .unwrap_or(0)
        });
        qr.apply_mask(mask);
        qr.draw_format(mask);
        qr.mask = mask;
        qr
    }

    pub fn get(&self, x: usize, y: usize) -> bool {
        self.modules[y * self.size + x]
    }

    fn set_fn(&mut self, x: usize, y: usize, dark: bool) {
        self.modules[y * self.size + x] = dark;
        self.function[y * self.size + x] = true;
    }

    fn draw_function_patterns(&mut self) {
        let size = self.size;
        for i in 0..size {
            self.set_fn(6, i, i % 2 == 0);
            self.set_fn(i, 6, i % 2 == 0);
        }
        for (cx, cy) in [(3, 3), (size - 4, 3), (3, size - 4)] {
            for dy in -4i32..=4 {
                for dx in -4i32..=4 {
                    let (x, y) = (cx as i32 + dx, cy as i32 + dy);
                    if x < 0 || y < 0 || x >= size as i32 || y >= size as i32 {
                        continue;
                    }
                    let d = dx.abs().max(dy.abs());
                    self.set_fn(x as usize, y as usize, d != 2 && d != 4);
                }
            }
        }
        let pos = alignment_positions(self.version);
        let n = pos.len();
        for i in 0..n {
            for j in 0..n {
                if (i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0) {
                    continue; // overlaps a finder
                }
                for dy in -2i32..=2 {
                    for dx in -2i32..=2 {
                        let d = dx.abs().max(dy.abs());
                        self.set_fn((pos[i] as i32 + dx) as usize, (pos[j] as i32 + dy) as usize, d != 1);
                    }
                }
            }
        }
        // reserve format areas (real bits drawn after masking) and version info
        self.draw_format(0);
        if self.version >= 7 {
            let mut rem = self.version as u32;
            for _ in 0..12 {
                rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
            }
            let bits = (self.version as u32) << 12 | rem;
            for i in 0..18 {
                let bit = (bits >> i) & 1 != 0;
                let a = size - 11 + i % 3;
                let b = i / 3;
                self.set_fn(a, b, bit);
                self.set_fn(b, a, bit);
            }
        }
    }

    fn draw_format(&mut self, mask: u8) {
        let data = (self.ecc.format_bits() << 3) | mask as u32;
        let mut rem = data;
        for _ in 0..10 {
            rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        }
        let bits = ((data << 10) | rem) ^ 0x5412;
        let bit = |i: usize| (bits >> i) & 1 != 0;
        let size = self.size;
        for i in 0..=5 {
            self.set_fn(8, i, bit(i));
        }
        self.set_fn(8, 7, bit(6));
        self.set_fn(8, 8, bit(7));
        self.set_fn(7, 8, bit(8));
        for i in 9..15 {
            self.set_fn(14 - i, 8, bit(i));
        }
        for i in 0..8 {
            self.set_fn(size - 1 - i, 8, bit(i));
        }
        for i in 8..15 {
            self.set_fn(8, size - 15 + i, bit(i));
        }
        self.set_fn(8, size - 8, true);
    }

    fn draw_codewords(&mut self, data: &[u8]) {
        let size = self.size;
        let total = data.len() * 8;
        let mut i = 0;
        let mut right = size as i32 - 1;
        while right >= 1 {
            if right == 6 {
                right = 5;
            }
            for vert in 0..size {
                for j in 0..2 {
                    let x = (right - j) as usize;
                    let upward = (right + 1) & 2 == 0;
                    let y = if upward { size - 1 - vert } else { vert };
                    if !self.function[y * size + x] && i < total {
                        self.modules[y * size + x] = (data[i >> 3] >> (7 - (i & 7))) & 1 != 0;
                        i += 1;
                    }
                }
            }
            right -= 2;
        }
    }

    fn apply_mask(&mut self, mask: u8) {
        let size = self.size;
        for y in 0..size {
            for x in 0..size {
                if self.function[y * size + x] {
                    continue;
                }
                let invert = match mask {
                    0 => (x + y) % 2 == 0,
                    1 => y % 2 == 0,
                    2 => x % 3 == 0,
                    3 => (x + y) % 3 == 0,
                    4 => (x / 3 + y / 2) % 2 == 0,
                    5 => x * y % 2 + x * y % 3 == 0,
                    6 => (x * y % 2 + x * y % 3) % 2 == 0,
                    _ => ((x + y) % 2 + x * y % 3) % 2 == 0,
                };
                if invert {
                    self.modules[y * size + x] ^= true;
                }
            }
        }
    }

    /// The spec's mask penalty (rules N1 to N4).
    fn penalty(&self) -> u32 {
        let n = self.size;
        let mut score = 0u32;
        let line = |get: &dyn Fn(usize) -> bool| -> u32 {
            let mut s = 0;
            let mut run = 1;
            for i in 1..n {
                if get(i) == get(i - 1) {
                    run += 1;
                } else {
                    if run >= 5 {
                        s += 3 + (run - 5);
                    }
                    run = 1;
                }
            }
            if run >= 5 {
                s += 3 + (run - 5);
            }
            // finder-like 1011101 with four light modules on either side
            let pat = [true, false, true, true, true, false, true];
            for i in 0..n.saturating_sub(6) {
                if (0..7).all(|k| get(i + k) == pat[k]) {
                    let before = (1..=4).all(|k| i < k || !get(i - k));
                    let after = (7..11).all(|k| i + k >= n || !get(i + k));
                    if before || after {
                        s += 40;
                    }
                }
            }
            s as u32
        };
        for y in 0..n {
            score += line(&|x| self.get(x, y));
        }
        for x in 0..n {
            score += line(&|y| self.get(x, y));
        }
        for y in 0..n - 1 {
            for x in 0..n - 1 {
                let c = self.get(x, y);
                if c == self.get(x + 1, y) && c == self.get(x, y + 1) && c == self.get(x + 1, y + 1) {
                    score += 3;
                }
            }
        }
        let dark = self.modules.iter().filter(|m| **m).count();
        let percent = dark * 100 / (n * n);
        score += (percent.abs_diff(50) / 5) as u32 * 10;
        score
    }

    /// Rows of `#` (dark) and `.` (light), no quiet zone; handy for tests.
    pub fn to_ascii(&self) -> String {
        let mut s = String::new();
        for y in 0..self.size {
            for x in 0..self.size {
                s.push(if self.get(x, y) { '#' } else { '.' });
            }
            s.push('\n');
        }
        s
    }

    /// Two rows per line with half blocks, black on white via ANSI colours so
    /// it scans on dark and light terminals alike.
    pub fn to_terminal(&self, quiet: usize) -> String {
        let n = self.size as i64;
        let q = quiet as i64;
        let dark = |x: i64, y: i64| x >= 0 && y >= 0 && x < n && y < n && self.get(x as usize, y as usize);
        let mut s = String::new();
        let mut y = -q;
        while y < n + q {
            s.push_str("\x1b[30;47m");
            for x in -q..n + q {
                s.push(match (dark(x, y), dark(x, y + 1)) {
                    (true, true) => '█',
                    (true, false) => '▀',
                    (false, true) => '▄',
                    (false, false) => ' ',
                });
            }
            s.push_str("\x1b[0m\n");
            y += 2;
        }
        s
    }
}

/// Lets ANSI colours work in the classic Windows console.
#[cfg(windows)]
pub fn enable_ansi() {
    #[link(name = "kernel32")]
    extern "system" {
        fn GetStdHandle(n: u32) -> *mut core::ffi::c_void;
        fn GetConsoleMode(h: *mut core::ffi::c_void, mode: *mut u32) -> i32;
        fn SetConsoleMode(h: *mut core::ffi::c_void, mode: u32) -> i32;
    }
    const STD_OUTPUT_HANDLE: u32 = -11i32 as u32;
    const ENABLE_VIRTUAL_TERMINAL_PROCESSING: u32 = 0x4;
    // SAFETY: plain Win32 console calls on our own stdout handle.
    unsafe {
        let h = GetStdHandle(STD_OUTPUT_HANDLE);
        let mut mode = 0u32;
        if GetConsoleMode(h, &mut mode) != 0 {
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
}

#[cfg(not(windows))]
pub fn enable_ansi() {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reed_solomon_known_block() {
        // the "HELLO WORLD" 1-M example from the spec's annex
        let data = [32u8, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236, 17, 236, 17];
        let ec = rs_remainder(&data, &rs_generator(10));
        assert_eq!(ec, vec![196, 35, 39, 119, 235, 215, 231, 226, 93, 23]);
    }

    #[test]
    fn capacities_match_the_spec() {
        // total codewords per version (data + EC) must fill the symbol
        let total = [26, 44, 70, 100, 134, 172, 196, 242, 292, 346];
        for v in 1..=10 {
            for ecc in [Ecc::L, Ecc::M] {
                let (ec, groups) = blocks(v, ecc);
                let n: usize = groups.iter().map(|(c, d)| c * (d + ec)).sum();
                assert_eq!(n, total[v - 1], "version {v} {ecc:?}");
            }
        }
    }

    #[test]
    fn picks_smallest_version() {
        assert_eq!(QrCode::encode(b"hello").unwrap().version, 1);
        let url = b"http://192.168.100.200:8740/?k=abcdefghijklmnopqrstuvwxyz234567";
        let qr = QrCode::encode(url).unwrap();
        assert_eq!((qr.version, qr.ecc), (5, Ecc::M));
        assert_eq!(qr.size, 37);
        assert!(QrCode::encode(&[b'x'; 213]).is_some());
        assert!(QrCode::encode(&[b'x'; 300]).is_none());
    }

    /// Matrices produced by an independent encoder (python-qrcode 8) for the same
    /// input, version, error level and mask.
    #[test]
    fn matches_reference_encoder() {
        let qr = QrCode::encode_with(b"achclwss", 1, Ecc::M, Some(2));
        assert_eq!(qr.to_ascii(), include_str!("../tests/data/qr-v1-m-mask2.txt"));
        let url = b"http://192.168.1.20:8740/?k=abcdefghijklmnopqrstuvwxyz234567";
        let qr = QrCode::encode_with(url, 5, Ecc::M, Some(5));
        assert_eq!(qr.to_ascii(), include_str!("../tests/data/qr-v5-m-mask5.txt"));
        let long = [b'a'; 150];
        let qr = QrCode::encode_with(&long, 8, Ecc::L, Some(0));
        assert_eq!(qr.to_ascii(), include_str!("../tests/data/qr-v8-l-mask0.txt"));
        let qr = QrCode::encode_with(&long, 10, Ecc::M, Some(7));
        assert_eq!(qr.to_ascii(), include_str!("../tests/data/qr-v10-m-mask7.txt"));
    }

    #[test]
    fn terminal_rendering_has_quiet_zone() {
        let qr = QrCode::encode(b"hi").unwrap();
        let t = qr.to_terminal(2);
        assert_eq!(t.lines().count(), (qr.size + 4).div_ceil(2));
        assert!(t.contains('█'));
    }
}
