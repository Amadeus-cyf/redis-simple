#ifndef REDIS_SIMPLE_UTILS_LZF_H_
#define REDIS_SIMPLE_UTILS_LZF_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace redis_simple::utils {
// LZF, the compression RDB files use for strings (Redis' lzf_compress and
// lzf_decompress). The format is a sequence of literal runs (a control byte
// below 32, then that many bytes plus one) and back references (the top
// three bits hold the length minus two, with 7 meaning an extra length byte
// follows, and the rest the distance minus one, up to 8192 bytes back).

// Compresses with a hash table of earlier positions that is kept between
// calls, so compressing many small strings does not allocate each time.
class LzfCompressor {
 public:
  // Compresses `input` into `output`. Returns the compressed size, or 0 when
  // it does not fit in `output`. Like Redis, callers pass an output smaller
  // than the input, so 0 means compressing would not save space.
  size_t Compress(std::string_view input, std::span<char> output);

 private:
  // Positions plus one of earlier three-byte sequences, by hash; 0 is empty.
  std::vector<uint32_t> table_;
};

// Decompresses `input` into `output`, which must be exactly the original
// size. Returns false if the input is malformed, refers outside the data, or
// does not decompress to exactly `output.size()` bytes.
bool LzfDecompress(std::string_view input, std::span<char> output);
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_LZF_H_
