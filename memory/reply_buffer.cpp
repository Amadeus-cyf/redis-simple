#include "memory/reply_buffer.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace redis_simple::in_memory {
ReplyBuffer::ReplyBuffer(size_t buffer_size, size_t chunk_size)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
    : buf_(new char[buffer_size]),
      buf_capacity_(buffer_size),
      chunk_size_(std::max<size_t>(chunk_size, 1)) {}

ReplyBuffer::~ReplyBuffer() {
  // Unlink iteratively so a long list cannot overflow the stack.
  while (reply_head_ != nullptr) {
    reply_head_ = std::move(reply_head_->next);
  }
}

size_t ReplyBuffer::Append(const char* s, size_t len) {
  if (len == 0) {
    return 0;
  }
  const size_t buffered = AppendToBuffer(s, len);
  if (buffered < len) {
    AppendToList(s + buffered, len - buffered);
  }
  pending_bytes_ += len;
  return len;
}

size_t ReplyBuffer::Append(std::string&& reply) {
  const size_t length = reply.size();
  if (length < kMinMovedReplySize) {
    return Append(reply.data(), length);
  }
  PushNode(BufNode::CreateOwned(std::move(reply)));
  pending_bytes_ += length;
  return length;
}

size_t ReplyBuffer::AppendToBuffer(const char* s, size_t len) {
  // Preserve write order: once overflow nodes exist, new bytes must also go to
  // the list until earlier list data is drained.
  if (node_count_ != 0) {
    return 0;
  }
  const size_t nwritten = std::min(len, buf_capacity_ - size_);
  std::memcpy(buf_.get() + size_, s, nwritten);
  size_ += nwritten;
  return nwritten;
}

void ReplyBuffer::AppendToList(const char* s, size_t len) {
  if (reply_tail_ != nullptr && reply_tail_->Writable()) {
    const size_t nwritten =
        std::min(len, reply_tail_->capacity - reply_tail_->used);
    std::memcpy(reply_tail_->Data() + reply_tail_->used, s, nwritten);
    reply_tail_->used += nwritten;
    s += nwritten;
    len -= nwritten;
  }
  if (len > 0) {
    auto node = BufNode::CreateWritable(std::max(len, chunk_size_));
    std::memcpy(node->Data(), s, len);
    node->used = len;
    PushNode(std::move(node));
  }
}

void ReplyBuffer::FillBlocks(std::vector<iovec>* const blocks) {
  assert(blocks != nullptr);
  blocks->clear();
  blocks->reserve(node_count_ + (size_ > 0 ? 1 : 0));
  if (size_ > 0) {
    blocks->push_back({buf_.get() + sent_, size_ - sent_});
  }
  size_t offset = size_ > 0 ? 0 : sent_;
  for (BufNode* node = reply_head_.get(); node != nullptr;
       node = node->next.get()) {
    blocks->push_back({node->Data() + offset, node->used - offset});
    offset = 0;
  }
}

void ReplyBuffer::Consume(size_t nwritten) {
  pending_bytes_ -= std::min(nwritten, pending_bytes_);
  const size_t processed = ConsumeBuffer(nwritten);
  if (processed < nwritten) {
    ConsumeList(nwritten - processed);
  }
}

size_t ReplyBuffer::ConsumeBuffer(size_t nwritten) {
  if (size_ == 0) {
    return 0;
  }
  nwritten = std::min(nwritten, size_ - sent_);
  sent_ += nwritten;
  if (sent_ >= size_) {
    size_ = 0;
    sent_ = 0;
  }
  return nwritten;
}

void ReplyBuffer::ConsumeList(size_t nwritten) {
  while (nwritten > 0 && reply_head_ != nullptr) {
    assert(sent_ <= reply_head_->used);
    const size_t remaining = reply_head_->used - sent_;
    if (nwritten < remaining) {
      sent_ += nwritten;
      break;
    }
    sent_ = 0;
    nwritten -= remaining;
    PopFront();
  }
}

void ReplyBuffer::PushNode(std::unique_ptr<BufNode> node) {
  ++node_count_;
  reply_bytes_ += node->capacity;
  BufNode* const node_ptr = node.get();
  if (reply_head_ == nullptr) {
    reply_head_ = std::move(node);
  } else {
    reply_tail_->next = std::move(node);
  }
  reply_tail_ = node_ptr;
}

void ReplyBuffer::PopFront() {
  reply_bytes_ -= reply_head_->capacity;
  --node_count_;
  reply_head_ = std::move(reply_head_->next);
  if (reply_head_ == nullptr) {
    reply_tail_ = nullptr;
  }
}
}  // namespace redis_simple::in_memory
