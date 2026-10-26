#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace redis_simple::utils {
using SipHashKey = std::array<uint8_t, 16>;

namespace siphash_internal {
constexpr uint64_t RotateLeft(uint64_t value, int bits) {
  return (value << bits) | (value >> (64 - bits));
}

inline uint64_t LoadLittleEndian64(const uint8_t* bytes) {
  uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8) | bytes[index];
  }
  return value;
}

struct State {
  uint64_t v0;
  uint64_t v1;
  uint64_t v2;
  uint64_t v3;

  void Round() {
    v0 += v1;
    v1 = RotateLeft(v1, 13);
    v1 ^= v0;
    v0 = RotateLeft(v0, 32);
    v2 += v3;
    v3 = RotateLeft(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = RotateLeft(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = RotateLeft(v1, 17);
    v1 ^= v2;
    v2 = RotateLeft(v2, 32);
  }
};
}  // namespace siphash_internal

// SipHash-c-d keyed with a 128-bit key (Aumasson and Bernstein, 2012).
// Keyed hashing keeps attackers from precomputing colliding keys that would
// degrade hash tables into long chains.
template <int kCompressionRounds, int kFinalizationRounds>
uint64_t SipHash(std::string_view data, const SipHashKey& key) {
  const uint64_t k0 = siphash_internal::LoadLittleEndian64(key.data());
  const uint64_t k1 = siphash_internal::LoadLittleEndian64(key.data() + 8);
  siphash_internal::State state{
      k0 ^ 0x736f6d6570736575ULL, k1 ^ 0x646f72616e646f6dULL,
      k0 ^ 0x6c7967656e657261ULL, k1 ^ 0x7465646279746573ULL};

  const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
  const size_t length = data.size();
  const size_t full_words = length / 8;
  for (size_t word = 0; word < full_words; ++word) {
    const uint64_t message =
        siphash_internal::LoadLittleEndian64(bytes + (word * 8));
    state.v3 ^= message;
    for (int round = 0; round < kCompressionRounds; ++round) {
      state.Round();
    }
    state.v0 ^= message;
  }

  uint64_t last = static_cast<uint64_t>(length & 0xff) << 56;
  const uint8_t* tail = bytes + (full_words * 8);
  for (size_t index = 0; index < (length & 7); ++index) {
    last |= static_cast<uint64_t>(tail[index]) << (8 * index);
  }
  state.v3 ^= last;
  for (int round = 0; round < kCompressionRounds; ++round) {
    state.Round();
  }
  state.v0 ^= last;

  state.v2 ^= 0xff;
  for (int round = 0; round < kFinalizationRounds; ++round) {
    state.Round();
  }
  return state.v0 ^ state.v1 ^ state.v2 ^ state.v3;
}

// Process-wide random key for hash tables, generated once on first use.
const SipHashKey& HashSeed();

// Seeded SipHash-1-3, the variant Redis uses for its dictionaries.
inline uint64_t SeededHash(std::string_view data) {
  return SipHash<1, 3>(data, HashSeed());
}
}  // namespace redis_simple::utils
