#include "md5.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

// Rotate the bits of x left by n positions (bits falling off the left come
// back on the right).
inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

}  // namespace

std::array<uint8_t, 16> md5(const std::string& input) {
  // Per-round "how far to rotate" amounts, straight from the MD5 spec (RFC 1321).
  static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                            5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

  // 64 constants defined as floor(2^32 * abs(sin(i + 1))). Computing them
  // avoids typing a 64-number table (and typos). Built once, thread-safely,
  // by the lambda below (C++11 guarantees static locals initialise safely).
  static const std::array<uint32_t, 64> K = [] {
    std::array<uint32_t, 64> k{};
    for (int i = 0; i < 64; ++i)
      k[i] = static_cast<uint32_t>(std::floor(std::fabs(std::sin(i + 1.0)) * 4294967296.0));
    return k;
  }();

  // The four 32-bit "registers" that hold the running hash. Start values from the spec.
  uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

  // ---- Padding ----
  // MD5 works on 64-byte blocks. We append: one 0x80 byte, then zeros until the
  // length is 56 (mod 64), then the original length in BITS as 8 bytes.
  std::vector<uint8_t> msg(input.begin(), input.end());
  uint64_t bit_len = static_cast<uint64_t>(input.size()) * 8;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) msg.push_back(0x00);
  for (int i = 0; i < 8; ++i) msg.push_back(static_cast<uint8_t>(bit_len >> (8 * i)));

  // ---- Process each 64-byte block ----
  for (size_t offset = 0; offset < msg.size(); offset += 64) {
    // Split the block into sixteen 32-bit little-endian words.
    uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
      const uint8_t* p = &msg[offset + i * 4];
      M[i] = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
             (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    uint32_t A = a0, B = b0, C = c0, D = d0;
    for (int i = 0; i < 64; ++i) {
      uint32_t F;
      int g;
      if (i < 16)      { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
      else if (i < 48) { F = B ^ C ^ D;          g = (3 * i + 5) % 16; }
      else             { F = C ^ (B | ~D);       g = (7 * i) % 16; }
      F = F + A + K[i] + M[g];
      A = D;
      D = C;
      C = B;
      B = B + rotl(F, S[i]);
    }
    a0 += A; b0 += B; c0 += C; d0 += D;
  }

  // ---- Output the four registers as 16 little-endian bytes ----
  std::array<uint8_t, 16> out;
  const uint32_t regs[4] = {a0, b0, c0, d0};
  for (int r = 0; r < 4; ++r)
    for (int b = 0; b < 4; ++b) out[r * 4 + b] = static_cast<uint8_t>(regs[r] >> (8 * b));
  return out;
}

std::string to_hex(const std::array<uint8_t, 16>& digest) {
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (uint8_t byte : digest) {
    s.push_back(hex[byte >> 4]);
    s.push_back(hex[byte & 0x0f]);
  }
  return s;
}
