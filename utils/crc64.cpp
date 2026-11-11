#include "utils/crc64.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace redis_simple::utils {
namespace {
// The Jones polynomial, bit-reversed for the reflected algorithm.
constexpr uint64_t kReflectedPolynomial = 0x95ac9329ac4bc9b5ULL;
constexpr size_t kSlices = 8;

using Tables = std::array<std::array<uint64_t, 256>, kSlices>;

// Slice-by-8 tables: tables[0] advances the CRC by one byte, and
// tables[k] by a byte followed by k zero bytes, so eight bytes are folded in
// with eight lookups instead of eight dependent steps.
constexpr Tables MakeTables() {
  Tables tables{};
  for (uint64_t byte = 0; byte < 256; ++byte) {
    uint64_t crc = byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) != 0 ? (crc >> 1) ^ kReflectedPolynomial : crc >> 1;
    }
    tables[0][byte] = crc;
  }
  for (size_t slice = 1; slice < kSlices; ++slice) {
    for (size_t byte = 0; byte < 256; ++byte) {
      const uint64_t previous = tables[slice - 1][byte];
      tables[slice][byte] = tables[0][previous & 0xff] ^ (previous >> 8);
    }
  }
  return tables;
}

constexpr Tables kTables = MakeTables();
}  // namespace

uint64_t Crc64(uint64_t crc, std::string_view data) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
  size_t remaining = data.size();
  while (remaining >= kSlices) {
    uint64_t word = 0;
    for (size_t i = 0; i < kSlices; ++i) {
      word |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    crc ^= word;
    crc = kTables[7][crc & 0xff] ^ kTables[6][(crc >> 8) & 0xff] ^
          kTables[5][(crc >> 16) & 0xff] ^ kTables[4][(crc >> 24) & 0xff] ^
          kTables[3][(crc >> 32) & 0xff] ^ kTables[2][(crc >> 40) & 0xff] ^
          kTables[1][(crc >> 48) & 0xff] ^ kTables[0][crc >> 56];
    bytes += kSlices;
    remaining -= kSlices;
  }
  for (; remaining > 0; --remaining, ++bytes) {
    crc = kTables[0][(crc ^ *bytes) & 0xff] ^ (crc >> 8);
  }
  return crc;
}
}  // namespace redis_simple::utils
