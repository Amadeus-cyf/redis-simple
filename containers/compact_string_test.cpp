#include "containers/compact_string.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace redis_simple::containers {
namespace {
TEST(CompactStringTest, HoldsEmptyInlineAndHeapValues) {
  const CompactString empty;
  EXPECT_EQ(empty.View(), "");
  EXPECT_EQ(empty.Size(), 0U);

  const std::string inline_value(CompactString::kInlineCapacity, 'i');
  const CompactString short_string(inline_value);
  EXPECT_EQ(short_string.View(), inline_value);

  const std::string heap_value(CompactString::kInlineCapacity + 1, 'h');
  const CompactString long_string(heap_value);
  EXPECT_EQ(long_string.View(), heap_value);
  EXPECT_EQ(long_string.Size(), heap_value.size());

  const std::string binary("a\0b\xff", 4);
  EXPECT_EQ(CompactString(binary).View(), binary);
}

TEST(CompactStringTest, AssignMovesBetweenForms) {
  CompactString value;
  const std::string long_value(100, 'x');
  const std::string other_long(100, 'y');
  value.Assign(long_value);
  EXPECT_EQ(value.View(), long_value);
  value.Assign(other_long);  // same length: reuses the block
  EXPECT_EQ(value.View(), other_long);
  value.Assign(std::string(40, 'z'));
  EXPECT_EQ(value.View(), std::string(40, 'z'));
  value.Assign("short");
  EXPECT_EQ(value.View(), "short");
  value.Assign("");
  EXPECT_EQ(value.View(), "");
  value.Assign(long_value);
  EXPECT_EQ(value.View(), long_value);
}

// Out of line so the optimizer cannot follow constant offsets into every
// branch of View(): GCC 15 then warns that an offset past 16 indexes the
// inline buffer, on a path that cannot run.
[[gnu::noinline]] std::string_view Slice(const CompactString& value,
                                         size_t position, size_t length) {
  return value.View().substr(position, length);
}

TEST(CompactStringTest, AssignAcceptsAViewOfItsOwnBytes) {
  CompactString value(std::string(64, 'a') + std::string(64, 'b'));
  value.Assign(Slice(value, 32, 64));  // shorter, still on the heap
  EXPECT_EQ(value.View(), std::string(32, 'a') + std::string(32, 'b'));
  value.Assign(Slice(value, 0, 64));  // same length, overlapping
  EXPECT_EQ(value.View(), std::string(32, 'a') + std::string(32, 'b'));
  value.Assign(Slice(value, 28, 8));  // to inline
  EXPECT_EQ(value.View(), "aaaabbbb");
  value.Assign(Slice(value, 2, 4));  // inline to inline
  EXPECT_EQ(value.View(), "aabb");
}

TEST(CompactStringTest, CopiesAndMoves) {
  const std::string long_value(50, 'q');
  CompactString original(long_value);
  CompactString copy(original);
  EXPECT_EQ(copy.View(), long_value);
  CompactString moved(std::move(original));
  EXPECT_EQ(moved.View(), long_value);
  CompactString assigned("x");
  assigned = moved;
  EXPECT_EQ(assigned.View(), long_value);
  assigned = std::move(copy);
  EXPECT_EQ(assigned.View(), long_value);
  // Self-assignment, through a reference as code reaching it would be.
  const CompactString& same = assigned;
  assigned = same;
  EXPECT_EQ(assigned.View(), long_value);
}
}  // namespace
}  // namespace redis_simple::containers
