#include "utils/siphash.h"

#include <random>

namespace redis_simple::utils {
namespace {
SipHashKey GenerateKey() {
  std::random_device device;
  SipHashKey key{};
  for (size_t index = 0; index < key.size(); index += 4) {
    const uint32_t random = device();
    for (size_t byte = 0; byte < 4; ++byte) {
      key[index + byte] = static_cast<uint8_t>(random >> (8 * byte));
    }
  }
  return key;
}
}  // namespace

const SipHashKey& HashSeed() {
  static const SipHashKey key = GenerateKey();
  return key;
}
}  // namespace redis_simple::utils
