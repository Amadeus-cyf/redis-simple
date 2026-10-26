#include "memory/reply_buffer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace redis_simple::in_memory {
namespace {
constexpr size_t kExpectedSentAfterConsume = 5000 - (4096 - 2047) - 2000;

std::string BlockString(const iovec& block) {
  return {static_cast<const char*>(block.iov_base), block.iov_len};
}

std::unique_ptr<ReplyBuffer> MakeBufferWithMainBytes() {
  // Small sizes keep the overflow-list cases below easy to reach.
  auto buf = std::make_unique<ReplyBuffer>(4096, 1024);
  std::string s(2000, 'a');
  buf->Append(s.c_str(), 2000);
  return buf;
}

std::unique_ptr<ReplyBuffer> MakeBufferWithReplyList() {
  auto buf = MakeBufferWithMainBytes();
  std::string s1(4096, 'b');
  buf->Append(s1.c_str(), 4096);
  std::string s2(1000, 'c');
  buf->Append(s2.c_str(), 1000);
  std::string s3(24, 'c');
  buf->Append(s3.c_str(), 24);
  std::string s4(1024, 'c');
  buf->Append(s4.c_str(), 1000);
  std::string s5(1024, 'd');
  buf->Append(s5.c_str(), 1024);
  return buf;
}

}  // namespace

TEST(ReplyBufferTest, AddToBuf) {
  auto buf = MakeBufferWithMainBytes();
  ASSERT_EQ(buf->BufferSize(), 2000);
}

TEST(ReplyBufferTest, AddToReplyList) {
  auto buf = MakeBufferWithMainBytes();
  std::string s1(4096, 'b');
  size_t r = buf->Append(s1.c_str(), 4096);
  ASSERT_EQ(r, 4096);
  ASSERT_EQ(buf->SentLength(), 0);
  ASSERT_EQ(buf->BufferSize(), 4096);
  ASSERT_EQ(buf->ReplyCount(), 1);

  // Add a new node to the reply list.
  std::string s2(1000, 'c');
  r = buf->Append(s2.c_str(), 1000);
  ASSERT_EQ(r, 1000);
  ASSERT_EQ(buf->ReplyCount(), 2);

  // no new node created
  std::string s3(24, 'c');
  r = buf->Append(s3.c_str(), 24);
  ASSERT_EQ(r, 24);
  ASSERT_EQ(buf->ReplyCount(), 2);

  // Add a new node with available space remained.
  std::string s4(1024, 'c');
  r = buf->Append(s4.c_str(), 1000);
  ASSERT_EQ(r, 1000);
  ASSERT_EQ(buf->BufferSize(), 4096);
  ASSERT_EQ(buf->ReplyCount(), 3);

  // Partially append to the last node and add a new node for the remaining
  // memory.
  std::string s5(1024, 'd');
  r = buf->Append(s5.c_str(), 1024);
  ASSERT_EQ(r, 1024);
  ASSERT_EQ(buf->BufferSize(), 4096);
  ASSERT_EQ(buf->ReplyCount(), 4);

  std::vector<iovec> mem_vec;
  buf->FillBlocks(&mem_vec);
  ASSERT_EQ(mem_vec.size(), 5);
  ASSERT_EQ(BlockString(mem_vec[0]),
            std::string(2000, 'a').append(4096 - 2000, 'b'));
  ASSERT_EQ(mem_vec[0].iov_len, 4096);
  ASSERT_EQ(BlockString(mem_vec[1]), std::string(4096 - (4096 - 2000), 'b'));
  ASSERT_EQ(mem_vec[1].iov_len, 4096 - (4096 - 2000));
  ASSERT_EQ(BlockString(mem_vec[2]), std::string(1024, 'c'));
  ASSERT_EQ(mem_vec[2].iov_len, 1024);
  ASSERT_EQ(BlockString(mem_vec[3]),
            std::string(1000, 'c') + std::string(24, 'd'));
  ASSERT_EQ(mem_vec[3].iov_len, 1024);
  ASSERT_EQ(BlockString(mem_vec[4]), std::string(1000, 'd'));
  ASSERT_EQ(mem_vec[4].iov_len, 1000);
}

TEST(ReplyBufferTest, Consume) {
  auto buf = MakeBufferWithReplyList();
  EXPECT_EQ(buf->PendingBytes(), 9144);
  // Partially consume the main buffer.
  buf->Consume(2047);
  EXPECT_EQ(buf->PendingBytes(), 7097);
  ASSERT_EQ(buf->BufferSize(), 4096);
  ASSERT_EQ(buf->SentLength(), 2047);

  // Consume the rest of the main buffer and one list node.
  buf->Consume(5000);
  EXPECT_EQ(buf->PendingBytes(), 2097);
  ASSERT_EQ(buf->BufferSize(), 0);
  ASSERT_EQ(buf->SentLength(), kExpectedSentAfterConsume);
  ASSERT_EQ(buf->ReplyCount(), 3);

  std::vector<iovec> mem_vec;
  buf->FillBlocks(&mem_vec);
  ASSERT_EQ(mem_vec.size(), 3);
  ASSERT_EQ(BlockString(mem_vec[0]),
            std::string(1024 - kExpectedSentAfterConsume, 'c'));
  ASSERT_EQ(mem_vec[0].iov_len, 1024 - kExpectedSentAfterConsume);
  ASSERT_EQ(BlockString(mem_vec[1]),
            std::string(1000, 'c') + std::string(24, 'd'));
  ASSERT_EQ(mem_vec[1].iov_len, 1024);
  ASSERT_EQ(BlockString(mem_vec[2]), std::string(1000, 'd'));
  ASSERT_EQ(mem_vec[2].iov_len, 1000);
}

TEST(ReplyBufferTest, ConsumeListNodeInMultiplePartialWrites) {
  auto buf = MakeBufferWithReplyList();
  buf->Consume(4096);

  buf->Consume(300);
  ASSERT_EQ(buf->SentLength(), 300);
  std::vector<iovec> mem_vec;
  buf->FillBlocks(&mem_vec);
  ASSERT_EQ(mem_vec.size(), 4);
  ASSERT_EQ(mem_vec[0].iov_len, 1700);

  buf->Consume(400);
  ASSERT_EQ(buf->SentLength(), 700);
  buf->FillBlocks(&mem_vec);
  ASSERT_EQ(mem_vec.size(), 4);
  ASSERT_EQ(BlockString(mem_vec[0]), std::string(1300, 'b'));
  ASSERT_EQ(mem_vec[0].iov_len, 1300);
}

TEST(ReplyBufferTest, AppendFillsTailBeforeAddingNode) {
  ReplyBuffer buffer(16, 1024);
  const std::string head(16, 'a');
  buffer.Append(head.data(), head.size());
  const std::string partial(333, 'd');
  buffer.Append(partial.data(), partial.size());
  ASSERT_EQ(buffer.ReplyCount(), 1);

  const std::string s(5000, 'e');
  ASSERT_EQ(buffer.Append(s.data(), s.size()), 5000);
  // The tail chunk is topped up before one node holds the remainder.
  ASSERT_EQ(buffer.ReplyCount(), 2);

  std::vector<iovec> blocks;
  buffer.FillBlocks(&blocks);
  ASSERT_EQ(blocks.size(), 3);
  EXPECT_EQ(BlockString(blocks[0]), head);
  EXPECT_EQ(BlockString(blocks[1]), partial + std::string(1024 - 333, 'e'));
  EXPECT_EQ(BlockString(blocks[2]), std::string(5000 - (1024 - 333), 'e'));
}

TEST(ReplyBufferTest, MovesLargeReplyIntoOutputList) {
  ReplyBuffer buffer;
  std::string reply(8192, 'x');
  const auto original_address = reinterpret_cast<std::uintptr_t>(reply.data());

  EXPECT_EQ(buffer.Append(std::move(reply)), 8192);

  std::vector<iovec> blocks;
  buffer.FillBlocks(&blocks);
  ASSERT_EQ(blocks.size(), 1);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(blocks[0].iov_base),
            original_address);
  EXPECT_EQ(BlockString(blocks[0]), std::string(8192, 'x'));

  buffer.Consume(4096);
  buffer.FillBlocks(&blocks);
  ASSERT_EQ(blocks.size(), 1);
  EXPECT_EQ(blocks[0].iov_len, 4096);
  EXPECT_EQ(BlockString(blocks[0]), std::string(4096, 'x'));
}

TEST(ReplyBufferTest, CopiesSmallMovedRepliesIntoSharedChunks) {
  ReplyBuffer buffer(16, 1024);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(buffer.Append(std::string("+OK\r\n")), 5);
  }
  // 16 bytes fit in the main buffer; the remaining 484 share one chunk.
  EXPECT_EQ(buffer.ReplyCount(), 1);
  EXPECT_EQ(buffer.PendingBytes(), 500);

  std::vector<iovec> blocks;
  buffer.FillBlocks(&blocks);
  std::string sent;
  for (const auto& block : blocks) {
    sent.append(BlockString(block));
  }
  std::string expected;
  for (int i = 0; i < 100; ++i) {
    expected.append("+OK\r\n");
  }
  EXPECT_EQ(sent, expected);
}
}  // namespace redis_simple::in_memory
