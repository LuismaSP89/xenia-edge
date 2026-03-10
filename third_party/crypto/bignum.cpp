#include "third_party/crypto/bignum.h"

namespace bignum {

void BigNum::trim() {
  while (limbs.size() > 1 && limbs.back() == 0) {
    limbs.pop_back();
  }
}

BigNum BigNum::from_bytes_be(const uint8_t* data, size_t len) {
  BigNum r;
  // Number of 8-byte limbs, rounding up
  size_t n = (len + 7) / 8;
  r.limbs.resize(n, 0);

  // Read bytes big-endian into little-endian limbs
  for (size_t i = 0; i < len; i++) {
    size_t byte_pos = len - 1 - i;  // position from LSB
    r.limbs[byte_pos / 8] |= static_cast<uint64_t>(data[i])
                              << (8 * (byte_pos % 8));
  }

  r.trim();
  return r;
}

void BigNum::to_bytes_be(uint8_t* out, size_t len) const {
  std::memset(out, 0, len);
  for (size_t i = 0; i < len; i++) {
    size_t byte_pos = len - 1 - i;  // position from LSB
    size_t li = byte_pos / 8;
    if (li < limbs.size()) {
      out[i] = static_cast<uint8_t>(limbs[li] >> (8 * (byte_pos % 8)));
    }
  }
}

int BigNum::compare(const BigNum& a, const BigNum& b) {
  size_t an = a.limbs.size(), bn = b.limbs.size();
  size_t n = std::max(an, bn);
  for (size_t i = n; i > 0; i--) {
    uint64_t al = (i - 1 < an) ? a.limbs[i - 1] : 0;
    uint64_t bl = (i - 1 < bn) ? b.limbs[i - 1] : 0;
    if (al < bl) return -1;
    if (al > bl) return 1;
  }
  return 0;
}

BigNum BigNum::sub(const BigNum& a, const BigNum& b) {
  // Assumes a >= b
  BigNum r;
  size_t n = a.limbs.size();
  r.limbs.resize(n, 0);
  uint64_t borrow = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t bl = (i < b.limbs.size()) ? b.limbs[i] : 0;
    __uint128_t diff =
        static_cast<__uint128_t>(a.limbs[i]) - bl - borrow;
    r.limbs[i] = static_cast<uint64_t>(diff);
    borrow = (diff >> 127) ? 1 : 0;  // Check if underflow (high bit set)
  }
  r.trim();
  return r;
}

BigNum BigNum::mul(const BigNum& a, const BigNum& b) {
  size_t an = a.limbs.size(), bn = b.limbs.size();
  BigNum r;
  r.limbs.resize(an + bn, 0);

  for (size_t i = 0; i < an; i++) {
    uint64_t carry = 0;
    for (size_t j = 0; j < bn; j++) {
      __uint128_t prod = static_cast<__uint128_t>(a.limbs[i]) * b.limbs[j] +
                         r.limbs[i + j] + carry;
      r.limbs[i + j] = static_cast<uint64_t>(prod);
      carry = static_cast<uint64_t>(prod >> 64);
    }
    r.limbs[i + bn] += carry;
  }

  r.trim();
  return r;
}

// Knuth Algorithm D: multi-precision division, returns remainder
BigNum BigNum::mod(const BigNum& a, const BigNum& m) {
  if (compare(a, m) < 0) return a;

  size_t n = m.limbs.size();
  size_t total = a.limbs.size();

  if (n == 0 || (n == 1 && m.limbs[0] == 0)) {
    return BigNum();  // division by zero guard
  }

  // Single-limb divisor fast path
  if (n == 1) {
    uint64_t d = m.limbs[0];
    uint64_t rem = 0;
    for (size_t i = total; i > 0; i--) {
      __uint128_t cur = (static_cast<__uint128_t>(rem) << 64) | a.limbs[i - 1];
      rem = static_cast<uint64_t>(cur % d);
    }
    BigNum r;
    r.limbs = {rem};
    r.trim();
    return r;
  }

  // Normalize: shift so that the MSB of the divisor's top limb is set
  int shift = 0;
  uint64_t top = m.limbs[n - 1];
  if (top != 0) {
    shift = __builtin_clzll(top);
  }

  // Create normalized copies
  BigNum u, v;
  // u = a << shift, with one extra limb
  u.limbs.resize(total + 1, 0);
  if (shift > 0) {
    uint64_t carry = 0;
    for (size_t i = 0; i < total; i++) {
      __uint128_t val = (static_cast<__uint128_t>(a.limbs[i]) << shift) | carry;
      u.limbs[i] = static_cast<uint64_t>(val);
      carry = static_cast<uint64_t>(val >> 64);
    }
    u.limbs[total] = carry;
  } else {
    for (size_t i = 0; i < total; i++) u.limbs[i] = a.limbs[i];
    u.limbs[total] = 0;
  }

  v.limbs.resize(n, 0);
  if (shift > 0) {
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
      __uint128_t val = (static_cast<__uint128_t>(m.limbs[i]) << shift) | carry;
      v.limbs[i] = static_cast<uint64_t>(val);
      carry = static_cast<uint64_t>(val >> 64);
    }
  } else {
    v.limbs = m.limbs;
  }

  uint64_t vn_1 = v.limbs[n - 1];
  uint64_t vn_2 = (n >= 2) ? v.limbs[n - 2] : 0;

  // Main loop: for each quotient digit position
  for (size_t j = total; j >= n; j--) {
    // Estimate quotient digit
    __uint128_t num_top =
        (static_cast<__uint128_t>(u.limbs[j]) << 64) | u.limbs[j - 1];
    __uint128_t qhat = num_top / vn_1;
    __uint128_t rhat = num_top % vn_1;

    // Refine estimate
    while (qhat > 0xFFFFFFFFFFFFFFFFULL ||
           qhat * vn_2 >
               ((rhat << 64) | u.limbs[j - 2])) {
      qhat--;
      rhat += vn_1;
      if (rhat > 0xFFFFFFFFFFFFFFFFULL) break;
    }

    // Multiply and subtract: u[j-n..j] -= qhat * v[0..n-1]
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
      __uint128_t prod =
          static_cast<__uint128_t>(static_cast<uint64_t>(qhat)) * v.limbs[i] +
          carry;
      uint64_t prod_lo = static_cast<uint64_t>(prod);
      carry = static_cast<uint64_t>(prod >> 64);
      uint64_t u_val = u.limbs[j - n + i];
      u.limbs[j - n + i] = u_val - prod_lo;
      if (u_val < prod_lo) carry++;
    }
    int64_t final_diff =
        static_cast<int64_t>(u.limbs[j]) - static_cast<int64_t>(carry);
    u.limbs[j] = static_cast<uint64_t>(final_diff);

    // If we subtracted too much, add back
    if (final_diff < 0) {
      uint64_t carry = 0;
      for (size_t i = 0; i < n; i++) {
        __uint128_t sum = static_cast<__uint128_t>(u.limbs[j - n + i]) +
                          v.limbs[i] + carry;
        u.limbs[j - n + i] = static_cast<uint64_t>(sum);
        carry = static_cast<uint64_t>(sum >> 64);
      }
      u.limbs[j] += carry;
    }
  }

  // Remainder is u[0..n-1] >> shift (un-normalize)
  BigNum r;
  r.limbs.resize(n, 0);
  if (shift > 0) {
    uint64_t carry = 0;
    for (size_t i = n; i > 0; i--) {
      __uint128_t val =
          (static_cast<__uint128_t>(carry) << 64) | u.limbs[i - 1];
      r.limbs[i - 1] = static_cast<uint64_t>(val >> shift);
      carry = u.limbs[i - 1] & ((1ULL << shift) - 1);
    }
  } else {
    for (size_t i = 0; i < n; i++) r.limbs[i] = u.limbs[i];
  }

  r.trim();
  return r;
}

BigNum BigNum::modexp(const BigNum& base, uint32_t exp, const BigNum& mod_val) {
  // Left-to-right binary square-and-multiply
  BigNum result;
  result.limbs = {1};

  // Find highest set bit
  if (exp == 0) {
    return mod(result, mod_val);
  }

  int highest_bit = 31 - __builtin_clz(exp);

  BigNum b = mod(base, mod_val);

  for (int i = highest_bit; i >= 0; i--) {
    result = mod(mul(result, result), mod_val);
    if ((exp >> i) & 1) {
      result = mod(mul(result, b), mod_val);
    }
  }

  return result;
}

}  // namespace bignum
