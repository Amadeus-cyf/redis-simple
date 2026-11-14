#include "containers/dynamic_buffer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "utils/check.h"

namespace redis_simple::containers {
namespace {
constexpr size_t kInitialCapacity = 4096;
// The most capacity an empty buffer keeps. Without a limit, one large
// request would hold its size for as long as its connection lives; Redis
// shrinks idle query buffers for the same reason
// (clientsCronResizeQueryBuffer).
constexpr size_t kMaxIdleCapacity = size_t{64} * 1024;
}  // namespace

DynamicBuffer::DynamicBuffer()
    // Bytes are always written before they are read, so skip zero-filling.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
    : buffer_(new char[kInitialCapacity]),
      capacity_(kInitialCapacity),
      size_(0),
      processed_(0) {}

void DynamicBuffer::Append(const char* buffer, size_t n) {
  if (n == 0) {
    return;
  }
  RS_CHECK(buffer != nullptr);
  Reserve(n);
  std::memcpy(buffer_.get() + size_, buffer, n);
  size_ += n;
}

char* DynamicBuffer::WritableTail(size_t n) {
  Reserve(n);
  return buffer_.get() + size_;
}

void DynamicBuffer::Commit(size_t n) {
  RS_CHECK(n <= capacity_ - size_);
  size_ += n;
}

void DynamicBuffer::Reserve(size_t n) {
  if (capacity_ - size_ >= n) {
    return;
  }
  if (processed_ > 0) {
    MoveUnreadToFront();
    if (capacity_ - size_ >= n) {
      return;
    }
  }
  RS_CHECK(n <= std::numeric_limits<size_t>::max() - size_);
  Resize(size_ + n);
}

void DynamicBuffer::Compact() {
  MoveUnreadToFront();
  if (size_ == 0 && capacity_ > kMaxIdleCapacity) {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
    buffer_.reset(new char[kInitialCapacity]);
    capacity_ = kInitialCapacity;
  }
}

void DynamicBuffer::MoveUnreadToFront() {
  if (processed_ == 0) {
    return;
  }
  const size_t unread = UnreadSize();
  std::memmove(buffer_.get(), buffer_.get() + processed_, unread);
  size_ = unread;
  processed_ = 0;
}

std::optional<std::string_view> DynamicBuffer::ReadLineView() {
  const size_t readable = UnreadSize();
  const auto* newline = static_cast<const char*>(
      std::memchr(buffer_.get() + processed_, '\n', readable));
  if (newline == nullptr) {
    return std::nullopt;
  }
  const char* line_end = newline;
  size_t delimiter_length = 1;
  if (line_end > buffer_.get() + processed_ && *(line_end - 1) == '\r') {
    --line_end;
    ++delimiter_length;
  }
  const size_t line_length = line_end - buffer_.get() - processed_;
  std::string_view line(buffer_.get() + processed_, line_length);
  processed_ += line_length + delimiter_length;
  return line;
}

std::string DynamicBuffer::ReadLine() {
  const std::optional<std::string_view> line = ReadLineView();
  return line.has_value() ? std::string(*line) : "";
}

void DynamicBuffer::Resize(size_t n) {
  // Grow geometrically so appending a large payload in chunks stays linear.
  const size_t doubled = capacity_ > std::numeric_limits<size_t>::max() / 2
                             ? std::numeric_limits<size_t>::max()
                             : capacity_ * 2;
  const size_t new_capacity = std::max(n, doubled);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
  std::unique_ptr<char[]> new_buffer(new char[new_capacity]);
  std::memcpy(new_buffer.get(), buffer_.get(), size_);
  buffer_ = std::move(new_buffer);
  capacity_ = new_capacity;
}

void DynamicBuffer::Clear() {
  size_ = 0;
  processed_ = 0;
}
}  // namespace redis_simple::containers
