#ifndef REDIS_SIMPLE_MEMORY_REPLY_BUFFER_H_
#define REDIS_SIMPLE_MEMORY_REPLY_BUFFER_H_

#include <sys/uio.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "memory/buf_node.h"

namespace redis_simple::in_memory {
class ReplyBuffer {
 public:
  // Matches Redis' PROTO_REPLY_CHUNK_BYTES.
  static constexpr size_t kDefaultBufferSize = size_t{16} * 1024;
  static constexpr size_t kDefaultChunkSize = size_t{16} * 1024;
  // Moved replies at least this large are queued without copying; smaller
  // ones are copied so pipelined replies share chunks.
  static constexpr size_t kMinMovedReplySize = size_t{4} * 1024;

  ReplyBuffer() : ReplyBuffer(kDefaultBufferSize, kDefaultChunkSize) {}
  ReplyBuffer(size_t buffer_size, size_t chunk_size);
  ReplyBuffer(const ReplyBuffer&) = delete;
  ReplyBuffer& operator=(const ReplyBuffer&) = delete;
  ~ReplyBuffer();

  const char* UnsentBuffer() const { return buf_.get() + sent_; }
  size_t UnsentLength() const { return size_ - sent_; }
  size_t SentLength() const { return sent_; }
  size_t ReplyCount() const { return node_count_; }
  size_t ReplyBytes() const { return reply_bytes_; }
  size_t PendingBytes() const { return pending_bytes_; }
  size_t BufferSize() const { return size_; }
  size_t Append(const char* s, size_t len);
  size_t Append(std::string&& reply);
  void Consume(size_t nwritten);
  void FillBlocks(std::vector<iovec>* blocks);
  bool Empty() const { return size_ == 0 && node_count_ == 0; }

 private:
  size_t AppendToBuffer(const char* s, size_t len);
  void AppendToList(const char* s, size_t len);
  size_t ConsumeBuffer(size_t nwritten);
  void ConsumeList(size_t nwritten);
  void PushNode(std::unique_ptr<BufNode> node);
  void PopFront();

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
  std::unique_ptr<char[]> buf_;
  size_t buf_capacity_;
  size_t chunk_size_;
  size_t size_{};
  // Sent bytes of the main buffer, or of the list head once it is empty.
  size_t sent_{};
  std::unique_ptr<BufNode> reply_head_;
  BufNode* reply_tail_{};
  size_t node_count_{};
  size_t reply_bytes_{};
  size_t pending_bytes_{};
};
}  // namespace redis_simple::in_memory

#endif  // REDIS_SIMPLE_MEMORY_REPLY_BUFFER_H_
