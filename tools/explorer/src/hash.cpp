// CRC-32, MD5, SHA-1 and SHA-256, small and dependency-free.
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "wetwaresuite/explorer/explorer.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::explorer {

namespace {

std::string hex(const unsigned char* d, std::size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        out += digits[d[i] >> 4];
        out += digits[d[i] & 15];
    }
    return out;
}

inline std::uint32_t rol(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline std::uint32_t ror(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// a Merkle-Damgard hash with 64-byte blocks: feed pieces, then finish
class Hasher {
public:
    explicit Hasher(std::string algo) : algo_(std::move(algo)) {
        if (algo_ == "md5") h_ = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
        else if (algo_ == "sha1") h_ = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        else if (algo_ == "sha256")
            h_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        else if (algo_ != "crc32") throw std::runtime_error("unknown checksum '" + algo_ + "' (crc32, md5, sha1 or sha256)");
    }
    void update(const unsigned char* d, std::size_t n) {
        if (algo_ == "crc32") {
            crc_ = crc32(d, n, crc_);
            return;
        }
        total_ += n;
        while (n) {
            std::size_t take = std::min<std::size_t>(64 - fill_, n);
            std::memcpy(block_ + fill_, d, take);
            fill_ += take;
            d += take;
            n -= take;
            if (fill_ == 64) {
                compress(block_);
                fill_ = 0;
            }
        }
    }
    std::string finish() {
        if (algo_ == "crc32") {
            char buf[9];
            std::snprintf(buf, sizeof buf, "%08x", crc_);
            return buf;
        }
        const std::uint64_t bits = total_ * 8;
        unsigned char pad[72] = {0x80};
        std::size_t padLen = (fill_ < 56 ? 56 - fill_ : 120 - fill_);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(algo_ == "md5" ? bits >> (8 * i) : bits >> (56 - 8 * i));
        update(pad, padLen);
        update(len, 8);
        unsigned char out[32];
        std::size_t words = algo_ == "md5" ? 4 : algo_ == "sha1" ? 5 : 8;
        for (std::size_t i = 0; i < words; ++i) {
            for (int b = 0; b < 4; ++b)
                out[i * 4 + b] = static_cast<unsigned char>(algo_ == "md5" ? h_[i] >> (8 * b) : h_[i] >> (24 - 8 * b));
        }
        return hex(out, words * 4);
    }

private:
    void compress(const unsigned char* p) {
        if (algo_ == "md5") md5(p);
        else if (algo_ == "sha1") sha1(p);
        else sha256(p);
    }
    void md5(const unsigned char* p) {
        static const std::uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
            0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
            0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
            0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
            0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
            0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | (std::uint32_t(p[i * 4 + 3]) << 24);
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) f = (b & c) | (~b & d), g = i;
            else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (b | ~d), g = (7 * i) % 16;
            std::uint32_t t = d;
            d = c;
            c = b;
            b = b + rol(a + f + K[i] + m[g], R[i]);
            a = t;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d;
    }
    void sha1(const unsigned char* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = (std::uint32_t(p[i * 4]) << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
    }
    void sha256(const unsigned char* p) {
        static const std::uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (std::uint32_t(p[i * 4]) << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = h + S1 + ch + K[i] + w[i];
            std::uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + mj;
            h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
    }

    std::string algo_;
    std::array<std::uint32_t, 8> h_{};
    unsigned char block_[64] = {};
    std::size_t fill_ = 0;
    std::uint64_t total_ = 0;
    std::uint32_t crc_ = 0;
};

std::string hashBytes(const std::string& algo, const std::string& bytes) {
    Hasher h(algo);
    h.update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return h.finish();
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t crc) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    const auto* p = static_cast<const unsigned char*>(data);
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::string md5Hex(const std::string& bytes) { return hashBytes("md5", bytes); }
std::string sha1Hex(const std::string& bytes) { return hashBytes("sha1", bytes); }
std::string sha256Hex(const std::string& bytes) { return hashBytes("sha256", bytes); }

std::string checksumFile(const fs::path& p, const std::string& algorithm) {
    Hasher h(sysutil::lowerAscii(algorithm));
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + sysutil::fromPath(p));
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize n = in.gcount();
        if (n > 0) h.update(reinterpret_cast<const unsigned char*>(buf.data()), static_cast<std::size_t>(n));
    }
    return h.finish();
}

}  // namespace wetwaresuite::explorer
