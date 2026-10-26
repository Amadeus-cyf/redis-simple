#include "dynamic_buffer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace redis_simple::in_memory {
namespace {
constexpr size_t kInitialCapacity = 4096;
}  // namespace

DynamicBuffer::DynamicBuffer()
    // Bytes are always written before they are read, so skip zero-filling.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
    : buf_(new char[kInitialCapacity]),
      capacity_(kInitialCapacity),
      size_(0),
      processed_(0) {}

void DynamicBuffer::Append(const char* buffer, size_t n) {
  if (n == 0) {
    return;
  }
  if (buffer == nullptr) {
    throw std::invalid_argument("cannot append a null buffer");
  }
  Reserve(n);
  std::memcpy(buf_.get() + size_, buffer, n);
  size_ += n;
}

char* DynamicBuffer::WritableTail(size_t n) {
  Reserve(n);
  return buf_.get() + size_;
}

void DynamicBuffer::Commit(size_t n) {
  if (n > capacity_ - size_) {
    throw std::length_error("dynamic buffer commit exceeds capacity");
  }
  size_ += n;
}

void DynamicBuffer::Reserve(size_t n) {
  if (capacity_ - size_ >= n) {
    return;
  }
  if (processed_ > 0) {
    Compact();
    if (capacity_ - size_ >= n) {
      return;
    }
  }
  if (n > std::numeric_limits<size_t>::max() - size_) {
    throw std::length_error("dynamic buffer size overflow");
  }
  Resize(size_ + n);
}

void DynamicBuffer::Compact() {
  if (processed_ == 0) {
    return;
  }
  const size_t unread = UnreadSize();
  std::memmove(buf_.get(), buf_.get() + processed_, unread);
  size_ = unread;
  processed_ = 0;
}

std::optional<std::string_view> DynamicBuffer::ReadLineView() {
  const size_t readable = UnreadSize();
  const auto* newline = static_cast<const char*>(
      std::memchr(buf_.get() + processed_, '\n', readable));
  if (newline == nullptr) {
    return std::nullopt;
  }
  const char* line_end = newline;
  size_t delimiter_length = 1;
  if (line_end > buf_.get() + processed_ && *(line_end - 1) == '\r') {
    --line_end;
    ++delimiter_length;
  }
  const size_t line_length = line_end - buf_.get() - processed_;
  std::string_view line(buf_.get() + processed_, line_length);
  processed_ += line_length + delimiter_length;
  return line;
}

std::string DynamicBuffer::ReadLine() {
  const auto line = ReadLineView();
  return line.has_value() ? std::string(*line) : "";
}

void DynamicBuffer::Resize(size_t n) {
  // Grow geometrically so appending a large payload in chunks stays linear.
  const size_t doubled = capacity_ > std::numeric_limits<size_t>::max() / 2
                             ? std::numeric_limits<size_t>::max()
                             : capacity_ * 2;
  const size_t new_capacity = std::max(n, doubled);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
  std::unique_ptr<char[]> new_buf(new char[new_capacity]);
  std::memcpy(new_buf.get(), buf_.get(), size_);
  buf_ = std::move(new_buf);
  capacity_ = new_capacity;
}

void DynamicBuffer::Clear() {
  size_ = 0;
  processed_ = 0;
}
}  // namespace redis_simple::in_memory
