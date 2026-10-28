#include "memory/dynamic_buffer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace redis_simple::in_memory {
namespace {
std::array<char, 4096> MakeLineBuffer() {
  std::array<char, 4096> buf{};
  char c = 'a';
  for (size_t i = 0; i < buf.size(); ++i) {
    if ((i + 1) % 1024 == 0) {
      ++c;
      buf[i] = '\n';
    } else {
      buf[i] = c;
    }
  }
  return buf;
}

std::unique_ptr<DynamicBuffer> MakeFilledBuffer() {
  auto buffer = std::make_unique<DynamicBuffer>();
  const auto buf = MakeLineBuffer();
  buffer->Append(buf.data(), buf.size());
  return buffer;
}

std::unique_ptr<DynamicBuffer> MakeTrimmedBuffer() {
  auto buffer = MakeFilledBuffer();
  buffer->ReadLine();
  buffer->ReadLine();
  buffer->Compact();
  buffer->ReadLine();
  buffer->Compact();
  return buffer;
}
}  // namespace

TEST(DynamicBufferTest, Write) {
  auto buffer = MakeFilledBuffer();
  ASSERT_EQ(buffer->Capacity(), 4096);
  ASSERT_EQ(buffer->Size(), 4096);
  ASSERT_EQ(buffer->Consumed(), 0);
}

TEST(DynamicBufferTest, ProcessInline) {
  auto buffer = MakeFilledBuffer();
  const std::string& s = buffer->ReadLine();
  ASSERT_EQ(s.length(), 1023);
  ASSERT_EQ(s, std::string(1023, 'a'));
  ASSERT_EQ(buffer->Capacity(), 4096);
  ASSERT_EQ(buffer->Size(), 4096);
  ASSERT_EQ(buffer->Consumed(), 1024);

  const std::string& s1 = buffer->ReadLine();
  ASSERT_EQ(s1.length(), 1023);
  ASSERT_EQ(s1, std::string(1023, 'b'));
  ASSERT_EQ(buffer->Capacity(), 4096);
  ASSERT_EQ(buffer->Size(), 4096);
  ASSERT_EQ(buffer->Consumed(), 2048);

  buffer->Compact();
  ASSERT_EQ(buffer->Capacity(), 4096);
  ASSERT_EQ(buffer->Size(), 2048);
  ASSERT_EQ(buffer->Consumed(), 0);

  const std::string& s2 = buffer->ReadLine();
  ASSERT_EQ(s2.length(), 1023);
  ASSERT_EQ(s2, std::string(1023, 'c'));
}

TEST(DynamicBufferTest, ReadLineViewAvoidsCopying) {
  DynamicBuffer buffer;
  buffer.Append("PING one\r\nPING two", 18);

  const std::string_view first = buffer.ReadLineView().value_or("");
  EXPECT_EQ(first, "PING one");
  EXPECT_EQ(buffer.Consumed(), 10);

  EXPECT_FALSE(buffer.ReadLineView().has_value());
  buffer.Append("\n", 1);
  const std::string_view second = buffer.ReadLineView().value_or("");
  EXPECT_EQ(second, "PING two");
}

TEST(DynamicBufferTest, TrimProcessed) {
  const auto buffer = MakeTrimmedBuffer();
  ASSERT_EQ(buffer->Capacity(), 4096);
  ASSERT_EQ(buffer->Size(), 1024);
  ASSERT_EQ(buffer->Consumed(), 0);
}

TEST(DynamicBufferTest, Resize) {
  auto buffer = MakeTrimmedBuffer();
  std::array<char, 8192> buf{};
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = (i + 1) % 1024 == 0 ? '\n' : 'c';
  }
  ASSERT_EQ(buffer->Size(), 1024);
  buffer->Append(buf.data(), buf.size());
  ASSERT_EQ(buffer->Capacity(), 9216);
  ASSERT_EQ(buffer->Size(), 9216);
  ASSERT_EQ(buffer->Consumed(), 0);
}

TEST(DynamicBufferTest, GrowsGeometricallyForLargePayloads) {
  DynamicBuffer buffer;
  const std::string chunk(size_t{16} * 1024, 'x');
  size_t growths = 0;
  size_t capacity = buffer.Capacity();
  for (int i = 0; i < 512; ++i) {
    buffer.Append(chunk.data(), chunk.size());
    if (buffer.Capacity() != capacity) {
      ++growths;
      capacity = buffer.Capacity();
    }
  }
  EXPECT_EQ(buffer.Size(), chunk.size() * 512);
  // 8 MiB from a 4 KiB start takes about 11 doublings.
  EXPECT_LE(growths, 12);
}

TEST(DynamicBufferTest, WritableTailCommitsWrittenBytes) {
  DynamicBuffer buffer;
  buffer.Append("ab", 2);
  char* const tail = buffer.WritableTail(8192);
  ASSERT_GE(buffer.Capacity() - buffer.Size(), 8192);
  constexpr std::string_view kTail = "cd";
  std::ranges::copy(kTail, tail);
  buffer.Commit(kTail.size());
  EXPECT_EQ(buffer.View(), "abcd");
}

TEST(DynamicBufferTest, ReusesConsumedCapacityBeforeGrowing) {
  DynamicBuffer buffer;
  const std::string first(3000, 'a');
  buffer.Append(first.data(), first.size());
  buffer.Consume(2500);

  const std::string second(2000, 'b');
  buffer.Append(second.data(), second.size());

  EXPECT_EQ(buffer.Capacity(), 4096);
  EXPECT_EQ(buffer.Consumed(), 0);
  EXPECT_EQ(buffer.Size(), 2500);
  EXPECT_EQ(buffer.View(), std::string(500, 'a') + second);
}
TEST(DynamicBufferDeathTest, RejectsCommitBeyondCapacity) {
  DynamicBuffer buffer;
  buffer.Append("ab", 2);
  EXPECT_DEATH(buffer.Commit(buffer.Capacity()), "Check failed");
}
}  // namespace redis_simple::in_memory
