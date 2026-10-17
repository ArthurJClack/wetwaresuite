// Whole numbers of any size (base 2^32 limbs) and exact fractions over them.
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "wetwaresuite/calc/calc.hpp"

namespace wetwaresuite::calc {

namespace {

using Limbs = std::vector<std::uint32_t>;

void trimLimbs(Limbs& m) {
    while (!m.empty() && m.back() == 0) m.pop_back();
}

int cmpMag(const Limbs& a, const Limbs& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (std::size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

Limbs addMag(const Limbs& a, const Limbs& b) {
    const Limbs& big = a.size() >= b.size() ? a : b;
    const Limbs& small = a.size() >= b.size() ? b : a;
    Limbs out(big.size() + 1, 0);
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < big.size(); ++i) {
        std::uint64_t s = static_cast<std::uint64_t>(big[i]) + (i < small.size() ? small[i] : 0) + carry;
        out[i] = static_cast<std::uint32_t>(s);
        carry = s >> 32;
    }
    out[big.size()] = static_cast<std::uint32_t>(carry);
    trimLimbs(out);
    return out;
}

// a - b with |a| >= |b|
Limbs subMag(const Limbs& a, const Limbs& b) {
    Limbs out(a.size(), 0);
    std::int64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::int64_t d = static_cast<std::int64_t>(a[i]) - (i < b.size() ? b[i] : 0) - borrow;
        borrow = d < 0 ? 1 : 0;
        if (d < 0) d += (std::int64_t{1} << 32);
        out[i] = static_cast<std::uint32_t>(d);
    }
    trimLimbs(out);
    return out;
}

Limbs mulMag(const Limbs& a, const Limbs& b) {
    if (a.empty() || b.empty()) return {};
    Limbs out(a.size() + b.size(), 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < b.size(); ++j) {
            std::uint64_t t = static_cast<std::uint64_t>(a[i]) * b[j] + out[i + j] + carry;
            out[i + j] = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
        std::size_t k = i + b.size();
        while (carry) {
            std::uint64_t t = static_cast<std::uint64_t>(out[k]) + carry;
            out[k++] = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
    }
    trimLimbs(out);
    return out;
}

// divides by a single limb in place, returns the remainder
std::uint32_t divSmall(Limbs& a, std::uint32_t d) {
    std::uint64_t rem = 0;
    for (std::size_t i = a.size(); i-- > 0;) {
        std::uint64_t cur = (rem << 32) | a[i];
        a[i] = static_cast<std::uint32_t>(cur / d);
        rem = cur % d;
    }
    trimLimbs(a);
    return static_cast<std::uint32_t>(rem);
}

std::size_t bitLength(const Limbs& m) {
    if (m.empty()) return 0;
    std::size_t n = (m.size() - 1) * 32;
    std::uint32_t top = m.back();
    while (top) {
        ++n;
        top >>= 1;
    }
    return n;
}

Limbs shlMag(const Limbs& a, std::size_t n) {
    if (a.empty()) return {};
    const std::size_t words = n / 32, bits = n % 32;
    Limbs out(a.size() + words + 1, 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::uint64_t v = static_cast<std::uint64_t>(a[i]) << bits;
        out[i + words] |= static_cast<std::uint32_t>(v);
        out[i + words + 1] |= static_cast<std::uint32_t>(v >> 32);
    }
    trimLimbs(out);
    return out;
}

Limbs shrMag(const Limbs& a, std::size_t n) {
    const std::size_t words = n / 32, bits = n % 32;
    if (words >= a.size()) return {};
    Limbs out(a.size() - words, 0);
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint64_t v = a[i + words];
        if (i + words + 1 < a.size()) v |= static_cast<std::uint64_t>(a[i + words + 1]) << 32;
        out[i] = static_cast<std::uint32_t>(v >> bits);
    }
    trimLimbs(out);
    return out;
}

// Knuth's algorithm D (TAOCP 4.3.1): q = a / b, r = a % b for magnitudes, b has 2+ limbs.
void divMagKnuth(const Limbs& a, const Limbs& b, Limbs& q, Limbs& r) {
    const std::size_t n = b.size(), m = a.size() - n;
    int s = 0;
    for (std::uint32_t top = b.back(); !(top & 0x80000000u); top <<= 1) ++s;
    Limbs v = shlMag(b, static_cast<std::size_t>(s));
    Limbs u = shlMag(a, static_cast<std::size_t>(s));
    u.resize(a.size() + 1, 0);
    v.resize(n, 0);
    q.assign(m + 1, 0);
    const std::uint64_t base = std::uint64_t{1} << 32;
    for (std::size_t j = m + 1; j-- > 0;) {
        std::uint64_t num = (static_cast<std::uint64_t>(u[j + n]) << 32) | u[j + n - 1];
        std::uint64_t qhat = num / v[n - 1], rhat = num % v[n - 1];
        while (qhat >= base || qhat * v[n - 2] > ((rhat << 32) | u[j + n - 2])) {
            --qhat;
            rhat += v[n - 1];
            if (rhat >= base) break;
        }
        std::int64_t borrow = 0;
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < n; ++i) {
            std::uint64_t p = qhat * v[i] + carry;
            carry = p >> 32;
            std::int64_t t = static_cast<std::int64_t>(u[i + j]) - borrow - static_cast<std::int64_t>(p & 0xffffffffu);
            borrow = t < 0 ? 1 : 0;
            u[i + j] = static_cast<std::uint32_t>(t < 0 ? t + static_cast<std::int64_t>(base) : t);
        }
        std::int64_t t = static_cast<std::int64_t>(u[j + n]) - borrow - static_cast<std::int64_t>(carry);
        u[j + n] = static_cast<std::uint32_t>(t < 0 ? t + static_cast<std::int64_t>(base) : t);
        if (t < 0) {  // qhat was one too big: add v back
            --qhat;
            std::uint64_t c = 0;
            for (std::size_t i = 0; i < n; ++i) {
                std::uint64_t s2 = static_cast<std::uint64_t>(u[i + j]) + v[i] + c;
                u[i + j] = static_cast<std::uint32_t>(s2);
                c = s2 >> 32;
            }
            u[j + n] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(u[j + n]) + c);
        }
        q[j] = static_cast<std::uint32_t>(qhat);
    }
    trimLimbs(q);
    u.resize(n);
    trimLimbs(u);
    r = shrMag(u, static_cast<std::size_t>(s));
}

void divMag(const Limbs& a, const Limbs& b, Limbs& q, Limbs& r) {
    if (cmpMag(a, b) < 0) {
        q.clear();
        r = a;
        return;
    }
    if (b.size() == 1) {
        q = a;
        std::uint32_t rem = divSmall(q, b[0]);
        r = rem ? Limbs{rem} : Limbs{};
        return;
    }
    divMagKnuth(a, b, q, r);
}

}  // namespace

struct BigIntAccess {
    static Limbs& mag(BigInt& b) { return b.mag_; }
    static const Limbs& mag(const BigInt& b) { return b.mag_; }
    static bool& neg(BigInt& b) { return b.neg_; }
    static BigInt make(bool neg, Limbs m) {
        BigInt b;
        b.neg_ = neg;
        b.mag_ = std::move(m);
        b.trim();
        return b;
    }
};

BigInt::BigInt(std::int64_t v) {
    neg_ = v < 0;
    std::uint64_t u = neg_ ? (~static_cast<std::uint64_t>(v) + 1) : static_cast<std::uint64_t>(v);
    while (u) {
        mag_.push_back(static_cast<std::uint32_t>(u));
        u >>= 32;
    }
}

void BigInt::trim() {
    trimLimbs(mag_);
    if (mag_.empty()) neg_ = false;
}

BigInt BigInt::parse(const std::string& digits, int base) {
    BigInt out;
    std::size_t i = 0;
    bool neg = false;
    if (i < digits.size() && (digits[i] == '-' || digits[i] == '+')) neg = digits[i++] == '-';
    bool any = false;
    for (; i < digits.size(); ++i) {
        char c = digits[i];
        if (c == '_' || c == '\'' || c == ',') continue;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (d >= base) throw std::runtime_error(std::string("'") + c + "' isn't a digit in base " + std::to_string(base));
        any = true;
        // out = out * base + d
        std::uint64_t carry = static_cast<std::uint64_t>(d);
        for (auto& limb : out.mag_) {
            std::uint64_t t = static_cast<std::uint64_t>(limb) * static_cast<std::uint64_t>(base) + carry;
            limb = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
        if (carry) out.mag_.push_back(static_cast<std::uint32_t>(carry));
    }
    if (!any) throw std::runtime_error("a number needs digits");
    out.neg_ = neg;
    out.trim();
    return out;
}

std::size_t BigInt::bits() const { return bitLength(mag_); }

bool BigInt::fitsInt64() const {
    if (mag_.size() > 2) return false;
    std::uint64_t u = mag_.empty() ? 0 : mag_[0];
    if (mag_.size() == 2) u |= static_cast<std::uint64_t>(mag_[1]) << 32;
    return neg_ ? u <= (std::uint64_t{1} << 63) : u < (std::uint64_t{1} << 63);
}

std::int64_t BigInt::toInt64() const {
    std::uint64_t u = mag_.empty() ? 0 : mag_[0];
    if (mag_.size() >= 2) u |= static_cast<std::uint64_t>(mag_[1]) << 32;
    return neg_ ? static_cast<std::int64_t>(~u + 1) : static_cast<std::int64_t>(u);
}

double BigInt::toDouble() const {
    double v = 0;
    for (std::size_t i = mag_.size(); i-- > 0;) v = v * 4294967296.0 + mag_[i];
    return neg_ ? -v : v;
}

std::string BigInt::toString(int base) const {
    if (mag_.empty()) return "0";
    std::string out;
    Limbs m = mag_;
    if (base == 10) {
        while (!m.empty()) {
            std::uint32_t chunk = divSmall(m, 1000000000u);
            for (int k = 0; k < 9; ++k) {
                out += static_cast<char>('0' + chunk % 10);
                chunk /= 10;
                if (m.empty() && chunk == 0) break;
            }
        }
    } else {
        static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
        while (!m.empty()) out += digits[divSmall(m, static_cast<std::uint32_t>(base))];
    }
    while (out.size() > 1 && out.back() == '0') out.pop_back();
    if (neg_) out += '-';
    std::reverse(out.begin(), out.end());
    return out;
}

BigInt BigInt::operator-() const {
    BigInt r = *this;
    if (!r.mag_.empty()) r.neg_ = !r.neg_;
    return r;
}

BigInt operator+(const BigInt& a, const BigInt& b) {
    if (a.neg_ == b.neg_) return BigIntAccess::make(a.neg_, addMag(a.mag_, b.mag_));
    int c = cmpMag(a.mag_, b.mag_);
    if (c == 0) return BigInt();
    return c > 0 ? BigIntAccess::make(a.neg_, subMag(a.mag_, b.mag_)) : BigIntAccess::make(b.neg_, subMag(b.mag_, a.mag_));
}

BigInt operator-(const BigInt& a, const BigInt& b) { return a + (-b); }

BigInt operator*(const BigInt& a, const BigInt& b) { return BigIntAccess::make(a.neg_ != b.neg_, mulMag(a.mag_, b.mag_)); }

void BigInt::divmod(const BigInt& a, const BigInt& b, BigInt& q, BigInt& r) {
    if (b.isZero()) throw std::runtime_error("can't divide by zero");
    Limbs qm, rm;
    divMag(a.mag_, b.mag_, qm, rm);
    q = BigIntAccess::make(a.neg_ != b.neg_, std::move(qm));
    r = BigIntAccess::make(a.neg_, std::move(rm));
}

BigInt operator/(const BigInt& a, const BigInt& b) {
    BigInt q, r;
    BigInt::divmod(a, b, q, r);
    return q;
}

BigInt operator%(const BigInt& a, const BigInt& b) {
    BigInt q, r;
    BigInt::divmod(a, b, q, r);
    return r;
}

bool operator<(const BigInt& a, const BigInt& b) {
    if (a.neg_ != b.neg_) return a.neg_;
    int c = cmpMag(a.mag_, b.mag_);
    return a.neg_ ? c > 0 : c < 0;
}

int BigInt::compareAbs(const BigInt& a, const BigInt& b) { return cmpMag(a.mag_, b.mag_); }

BigInt BigInt::abs() const {
    BigInt r = *this;
    r.neg_ = false;
    return r;
}

BigInt BigInt::gcd(BigInt a, BigInt b) {
    a = a.abs();
    b = b.abs();
    while (!b.isZero()) {
        BigInt r = a % b;
        a = std::move(b);
        b = std::move(r);
    }
    return a;
}

BigInt BigInt::pow(const BigInt& base, std::uint64_t exp) {
    BigInt result(1), b = base;
    while (exp) {
        if (exp & 1) result = result * b;
        exp >>= 1;
        if (exp) b = b * b;
    }
    return result;
}

BigInt BigInt::shiftLeft(std::size_t n) const { return BigIntAccess::make(neg_, shlMag(mag_, n)); }

BigInt BigInt::shiftRight(std::size_t n) const {
    if (!neg_) return BigIntAccess::make(false, shrMag(mag_, n));
    // floor for negatives: -((|x| - 1) >> n) - 1
    BigInt m = abs() - BigInt(1);
    return -BigIntAccess::make(false, shrMag(BigIntAccess::mag(m), n)) - BigInt(1);
}

namespace {

// two's complement over `limbs` limbs
Limbs twos(const BigInt& x, std::size_t limbs) {
    Limbs m = BigIntAccess::mag(x);
    m.resize(limbs, 0);
    if (!x.negative()) return m;
    for (auto& l : m) l = ~l;
    for (auto& l : m) {
        if (++l != 0) break;
    }
    return m;
}

BigInt fromTwos(Limbs m) {
    if (m.empty() || !(m.back() & 0x80000000u)) return BigIntAccess::make(false, std::move(m));
    for (auto& l : m) l = ~l;
    for (auto& l : m) {
        if (++l != 0) break;
    }
    return BigIntAccess::make(true, std::move(m));
}

template <typename Op>
BigInt bitwise(const BigInt& a, const BigInt& b, Op op) {
    const std::size_t n = std::max(BigIntAccess::mag(a).size(), BigIntAccess::mag(b).size()) + 1;
    Limbs x = twos(a, n), y = twos(b, n), out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = op(x[i], y[i]);
    return fromTwos(std::move(out));
}

}  // namespace

BigInt BigInt::bitAnd(const BigInt& a, const BigInt& b) {
    return bitwise(a, b, [](std::uint32_t x, std::uint32_t y) { return x & y; });
}
BigInt BigInt::bitOr(const BigInt& a, const BigInt& b) {
    return bitwise(a, b, [](std::uint32_t x, std::uint32_t y) { return x | y; });
}
BigInt BigInt::bitXor(const BigInt& a, const BigInt& b) {
    return bitwise(a, b, [](std::uint32_t x, std::uint32_t y) { return x ^ y; });
}
BigInt BigInt::bitNot() const { return -*this - BigInt(1); }

BigInt BigInt::lowBits(std::size_t nbits) const {
    const std::size_t limbs = (nbits + 31) / 32;
    Limbs m = twos(*this, std::max(limbs, mag_.size() + 1));
    m.resize(limbs);
    if (nbits % 32 && !m.empty()) m.back() &= (std::uint32_t{1} << (nbits % 32)) - 1;
    return BigIntAccess::make(false, std::move(m));
}

std::size_t BigInt::popcount() const {
    std::size_t n = 0;
    for (auto l : mag_) {
        for (; l; l &= l - 1) ++n;
    }
    return n;
}

// ---- exact fractions -----------------------------------------------------------------------

namespace {

// past this many bits an exact result switches to floating point, so answers stay quick
constexpr std::size_t kMaxExactBits = 100000;

Number tooBig(const BigInt& n, const BigInt& d) {
    // keep the top 64 bits of each and put the rest in the exponent, so huge numbers don't become inf/inf
    const std::size_t en = n.bits(), ed = d.bits();
    const std::size_t sn = en > 64 ? en - 64 : 0, sd = ed > 64 ? ed - 64 : 0;
    double v = n.abs().shiftRight(sn).toDouble() / d.abs().shiftRight(sd).toDouble();
    v = std::ldexp(v, static_cast<int>(static_cast<long long>(sn) - static_cast<long long>(sd)));
    return Number::real(n.negative() != d.negative() ? -v : v);
}

}  // namespace

Number Number::real(double v) {
    Number n;
    n.exact = false;
    n.approx = v;
    return n;
}

Number Number::fraction(BigInt n, BigInt d) {
    if (d.isZero()) throw std::runtime_error("can't divide by zero");
    if (d.negative()) {
        n = -n;
        d = -d;
    }
    BigInt g = BigInt::gcd(n, d);
    if (!g.isZero() && g != BigInt(1)) {
        n = n / g;
        d = d / g;
    }
    if (n.bits() > kMaxExactBits || d.bits() > kMaxExactBits) return tooBig(n, d);
    Number r;
    r.num = std::move(n);
    r.den = std::move(d);
    return r;
}

double Number::toDouble() const {
    if (!exact) return approx;
    if (den == BigInt(1)) return num.toDouble();
    if (num.bits() < 1000 && den.bits() < 1000) return num.toDouble() / den.toDouble();
    return tooBig(num, den).approx;
}

Number add(const Number& a, const Number& b) {
    if (a.exact && b.exact) {
        if (a.den == b.den) return Number::fraction(a.num + b.num, a.den);
        return Number::fraction(a.num * b.den + b.num * a.den, a.den * b.den);
    }
    return Number::real(a.toDouble() + b.toDouble());
}

Number sub(const Number& a, const Number& b) { return add(a, neg(b)); }

Number mul(const Number& a, const Number& b) {
    if (a.exact && b.exact) return Number::fraction(a.num * b.num, a.den * b.den);
    return Number::real(a.toDouble() * b.toDouble());
}

Number div(const Number& a, const Number& b) {
    if (b.isZero()) throw std::runtime_error("can't divide by zero");
    if (a.exact && b.exact) return Number::fraction(a.num * b.den, a.den * b.num);
    return Number::real(a.toDouble() / b.toDouble());
}

Number neg(const Number& a) {
    Number r = a;
    if (r.exact) r.num = -r.num;
    else r.approx = -r.approx;
    return r;
}

int compare(const Number& a, const Number& b) {
    if (a.exact && b.exact) {
        BigInt l = a.num * b.den, r = b.num * a.den;
        return l == r ? 0 : (l < r ? -1 : 1);
    }
    double x = a.toDouble(), y = b.toDouble();
    return x == y ? 0 : (x < y ? -1 : 1);
}

}  // namespace wetwaresuite::calc
