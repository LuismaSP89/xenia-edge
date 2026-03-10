#ifndef BIGNUM_H_
#define BIGNUM_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace bignum {

class BigNum {
 public:
  // Limbs in little-endian order (limbs[0] is least significant)
  std::vector<uint64_t> limbs;

  BigNum() = default;

  static BigNum from_bytes_be(const uint8_t* data, size_t len);
  void to_bytes_be(uint8_t* out, size_t len) const;

  static BigNum modexp(const BigNum& base, uint32_t exp, const BigNum& mod);

 private:
  static BigNum mul(const BigNum& a, const BigNum& b);
  static BigNum mod(const BigNum& a, const BigNum& m);
  static int compare(const BigNum& a, const BigNum& b);
  static BigNum sub(const BigNum& a, const BigNum& b);
  void trim();
};

}  // namespace bignum

#endif  // BIGNUM_H_
