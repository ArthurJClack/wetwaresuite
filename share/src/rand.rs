//! Secrets from the operating system's CSPRNG: /dev/urandom on unix,
//! BCryptGenRandom on Windows. No fallback: if the OS can't give us random
//! bytes we refuse to share rather than use a guessable token.

use std::io;

#[cfg(unix)]
pub fn fill(buf: &mut [u8]) -> io::Result<()> {
    use std::io::Read;
    std::fs::File::open("/dev/urandom")?.read_exact(buf)
}

#[cfg(windows)]
pub fn fill(buf: &mut [u8]) -> io::Result<()> {
    #[link(name = "bcrypt")]
    extern "system" {
        fn BCryptGenRandom(alg: *mut core::ffi::c_void, buf: *mut u8, len: u32, flags: u32) -> i32;
    }
    const BCRYPT_USE_SYSTEM_PREFERRED_RNG: u32 = 0x2;
    for chunk in buf.chunks_mut(u32::MAX as usize) {
        // SAFETY: the pointer and length describe a valid, writable slice, and a
        // null algorithm handle is allowed with BCRYPT_USE_SYSTEM_PREFERRED_RNG.
        let status = unsafe {
            BCryptGenRandom(std::ptr::null_mut(), chunk.as_mut_ptr(), chunk.len() as u32, BCRYPT_USE_SYSTEM_PREFERRED_RNG)
        };
        if status != 0 {
            return Err(io::Error::other(format!("BCryptGenRandom failed: {status:#x}")));
        }
    }
    Ok(())
}

#[cfg(not(any(unix, windows)))]
pub fn fill(_buf: &mut [u8]) -> io::Result<()> {
    Err(io::Error::other("no OS random source on this platform"))
}

pub fn bytes<const N: usize>() -> io::Result<[u8; N]> {
    let mut b = [0u8; N];
    fill(&mut b)?;
    Ok(b)
}

/// A 160-bit access token as 32 base32 characters (lowercase, URL safe).
pub fn token() -> io::Result<String> {
    const ALPHABET: &[u8; 32] = b"abcdefghijklmnopqrstuvwxyz234567";
    let raw = bytes::<20>()?;
    let mut out = String::with_capacity(32);
    let mut acc: u64 = 0;
    let mut bits = 0;
    for b in raw {
        acc = (acc << 8) | b as u64;
        bits += 8;
        while bits >= 5 {
            bits -= 5;
            out.push(ALPHABET[((acc >> bits) & 31) as usize] as char);
        }
    }
    Ok(out)
}

/// Pairing-code alphabet: no 0/O, 1/I/L, so it reads well aloud and on a phone.
pub const CODE_ALPHABET: &[u8] = b"23456789ABCDEFGHJKMNPQRSTUVWXYZ";

/// A 6-character pairing code (about 29.7 bits). It is only accepted by the
/// rate-limited pairing endpoint, never as a token.
pub fn code() -> io::Result<String> {
    let mut out = String::with_capacity(6);
    while out.len() < 6 {
        for b in bytes::<16>()? {
            // rejection sampling keeps every character equally likely
            let limit = 256 - 256 % CODE_ALPHABET.len();
            if (b as usize) < limit && out.len() < 6 {
                out.push(CODE_ALPHABET[b as usize % CODE_ALPHABET.len()] as char);
            }
        }
    }
    Ok(out)
}

/// Normalises what a person typed as a code: case, spaces and dashes.
pub fn normalize_code(input: &str) -> String {
    input.chars().filter(|c| !c.is_whitespace() && *c != '-').map(|c| c.to_ascii_uppercase()).collect()
}

/// True when `s` looks like a pairing code rather than a URL or address.
pub fn looks_like_code(s: &str) -> bool {
    let n = normalize_code(s);
    n.len() == 6 && n.bytes().all(|b| CODE_ALPHABET.contains(&b))
}

/// Compares secrets in time that depends only on their lengths.
pub fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for (x, y) in a.iter().zip(b) {
        diff |= x ^ y;
    }
    // black_box stops the optimiser from turning this into an early exit
    std::hint::black_box(diff) == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tokens_are_long_and_distinct() {
        let a = token().unwrap();
        let b = token().unwrap();
        assert_eq!(a.len(), 32);
        assert_ne!(a, b);
        assert!(a.bytes().all(|c| c.is_ascii_lowercase() || (b'2'..=b'7').contains(&c)));
    }

    #[test]
    fn codes_use_the_alphabet() {
        for _ in 0..50 {
            let c = code().unwrap();
            assert_eq!(c.len(), 6);
            assert!(looks_like_code(&c));
        }
        assert_eq!(normalize_code("ab3-k9z"), "AB3K9Z");
        assert!(!looks_like_code("http://x"));
        assert!(!looks_like_code("ABCDE"));
    }

    #[test]
    fn constant_time_compare() {
        assert!(constant_time_eq(b"secret", b"secret"));
        assert!(!constant_time_eq(b"secret", b"secreT"));
        assert!(!constant_time_eq(b"secret", b"secret2"));
        assert!(!constant_time_eq(b"", b"x"));
        assert!(constant_time_eq(b"", b""));
    }
}
