#ifndef REDIS_SIMPLE_BENCHMARKS_BUFFER_H_
#define REDIS_SIMPLE_BENCHMARKS_BUFFER_H_

#include <array>
#include <cstddef>

namespace redis_simple {
static constexpr size_t kBufferSize = 4096;
static std::array<char, kBufferSize> g_buffer{};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_BENCHMARKS_BUFFER_H_
