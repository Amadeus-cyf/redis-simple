#ifndef REDIS_SIMPLE_UTILS_SIPHASH_H_
#define REDIS_SIMPLE_UTILS_SIPHASH_H_

#include <array>
#include <cstddef>
#include <cstdint>
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

class State {
 public:
  State(uint64_t k0, uint64_t k1)
      : v0_(k0 ^ 0x736f6d6570736575ULL),
        v1_(k1 ^ 0x646f72616e646f6dULL),
        v2_(k0 ^ 0x6c7967656e657261ULL),
        v3_(k1 ^ 0x7465646279746573ULL) {}

  template <int kRounds>
  void Absorb(uint64_t message) {
    v3_ ^= message;
    for (int round = 0; round < kRounds; ++round) {
      Round();
    }
    v0_ ^= message;
  }

  template <int kRounds>
  uint64_t Finish() {
    v2_ ^= 0xff;
    for (int round = 0; round < kRounds; ++round) {
      Round();
    }
    return v0_ ^ v1_ ^ v2_ ^ v3_;
  }

 private:
  void Round() {
    v0_ += v1_;
    v1_ = RotateLeft(v1_, 13);
    v1_ ^= v0_;
    v0_ = RotateLeft(v0_, 32);
    v2_ += v3_;
    v3_ = RotateLeft(v3_, 16);
    v3_ ^= v2_;
    v0_ += v3_;
    v3_ = RotateLeft(v3_, 21);
    v3_ ^= v0_;
    v2_ += v1_;
    v1_ = RotateLeft(v1_, 17);
    v1_ ^= v2_;
    v2_ = RotateLeft(v2_, 32);
  }

  uint64_t v0_;
  uint64_t v1_;
  uint64_t v2_;
  uint64_t v3_;
};
}  // namespace siphash_internal

// SipHash-c-d keyed with a 128-bit key (Aumasson and Bernstein, 2012).
// Keyed hashing keeps attackers from precomputing colliding keys that would
// degrade hash tables into long chains.
template <int kCompressionRounds, int kFinalizationRounds>
uint64_t SipHash(std::string_view data, const SipHashKey& key) {
  const uint64_t k0 = siphash_internal::LoadLittleEndian64(key.data());
  const uint64_t k1 = siphash_internal::LoadLittleEndian64(key.data() + 8);
  siphash_internal::State state(k0, k1);

  const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
  const size_t length = data.size();
  const size_t full_words = length / 8;
  for (size_t word = 0; word < full_words; ++word) {
    state.Absorb<kCompressionRounds>(
        siphash_internal::LoadLittleEndian64(bytes + (word * 8)));
  }

  uint64_t last = static_cast<uint64_t>(length & 0xff) << 56;
  const uint8_t* tail = bytes + (full_words * 8);
  for (size_t index = 0; index < (length & 7); ++index) {
    last |= static_cast<uint64_t>(tail[index]) << (8 * index);
  }
  state.Absorb<kCompressionRounds>(last);
  return state.Finish<kFinalizationRounds>();
}

// Process-wide random key for hash tables, generated once on first use.
const SipHashKey& HashSeed();

// Seeded SipHash-1-3, the variant Redis uses for its dictionaries.
inline uint64_t SeededHash(std::string_view data) {
  return SipHash<1, 3>(data, HashSeed());
}
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_SIPHASH_H_
