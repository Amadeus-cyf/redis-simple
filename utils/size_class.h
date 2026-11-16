#ifndef REDIS_SIMPLE_UTILS_SIZE_CLASS_H_
#define REDIS_SIMPLE_UTILS_SIZE_CLASS_H_

#include <bit>
#include <cstddef>

namespace redis_simple::utils {
// The largest request AllocationSizeClass rounds up.
inline constexpr size_t kMaxAllocationSizeClassBytes = size_t{64} * 1024;

// The bytes mimalloc, the server's allocator, reserves for a request of
// `bytes`: multiples of 8 up to 64 bytes, then four sizes per power of two
// (80, 96, 112, 128, 160, ...) up to 64 KiB. A larger request is returned
// unchanged; mimalloc rounds those to pages, which is not modelled. A buffer
// that grows a few bytes at a time asks for this size, so the spare bytes are
// ones the allocator would have left unused anyway and the buffer is
// reallocated only when it outgrows them. The bundled mimalloc aligns to 16
// bytes, so it reserves 32, 48 and 64 bytes where this returns 24, 40 and 56:
// more, never less. With another allocator the result is still a valid size,
// only a less exact match.
constexpr size_t AllocationSizeClass(size_t bytes) {
  constexpr size_t kWordBytes = 8;
  constexpr size_t kMaxWordSteppedBytes = 64;
  constexpr size_t kClassesPerPowerOfTwo = 4;
  if (bytes > kMaxAllocationSizeClassBytes) {
    return bytes;
  }
  if (bytes <= kWordBytes) {
    return kWordBytes;
  }
  const size_t step = bytes <= kMaxWordSteppedBytes
                          ? kWordBytes
                          : std::bit_floor(bytes - 1) / kClassesPerPowerOfTwo;
  // `step` is a power of two.
  return (bytes + step - 1) & ~(step - 1);
}
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_SIZE_CLASS_H_
