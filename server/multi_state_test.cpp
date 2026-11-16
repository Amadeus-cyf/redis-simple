#include "server/multi_state.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "server/commands/command.h"

namespace redis_simple {
namespace {
using QueuedArgs = std::vector<std::string>;

// The queued commands as (command, arguments) in order.
std::vector<std::pair<const commands::Command*, QueuedArgs>> Queued(
    const MultiState& multi) {
  std::vector<std::pair<const commands::Command*, QueuedArgs>> queued;
  commands::CommandArgs args;
  multi.ForEach(&args, [&queued, &args](const commands::Command* command) {
    queued.emplace_back(command, QueuedArgs(args.begin(), args.end()));
    return true;
  });
  return queued;
}

void QueueSets(const size_t count, const std::string_view value,
               MultiState* const multi) {
  const commands::Command* const set_command = commands::Find("SET");
  for (size_t i = 0; i < count; ++i) {
    const std::string key = "key:" + std::to_string(i);
    ASSERT_TRUE(multi->Queue(set_command, {"SET", key, value}));
  }
}

TEST(MultiStateTest, ForEachReturnsEachCommandWithItsOwnArguments) {
  const commands::Command* const set_command = commands::Find("SET");
  const commands::Command* const get_command = commands::Find("GET");
  const commands::Command* const ping_command = commands::Find("PING");
  MultiState multi;
  multi.Begin();
  // The queue copies its arguments: the strings they view change afterwards.
  std::string key = "key";
  std::string value = "value";
  ASSERT_TRUE(multi.Queue(set_command, {"SET", key, value}));
  key = "abc";
  value = "";
  ASSERT_TRUE(multi.Queue(ping_command, {"PING"}));
  ASSERT_TRUE(multi.Queue(set_command, {"SET", key, value}));
  ASSERT_TRUE(multi.Queue(get_command, {"GET", key}));
  key = "xyz";

  const auto queued = Queued(multi);

  ASSERT_EQ(queued.size(), 4U);
  EXPECT_EQ(multi.Size(), 4U);
  EXPECT_EQ(queued[0].first, set_command);
  EXPECT_EQ(queued[0].second, (QueuedArgs{"SET", "key", "value"}));
  EXPECT_EQ(queued[1].first, ping_command);
  EXPECT_EQ(queued[1].second, (QueuedArgs{"PING"}));
  EXPECT_EQ(queued[2].first, set_command);
  EXPECT_EQ(queued[2].second, (QueuedArgs{"SET", "abc", ""}));
  EXPECT_EQ(queued[3].first, get_command);
  EXPECT_EQ(queued[3].second, (QueuedArgs{"GET", "abc"}));
}

TEST(MultiStateTest, ForEachStopsWhenRunReturnsFalse) {
  MultiState multi;
  multi.Begin();
  QueueSets(3, "v", &multi);
  commands::CommandArgs args;
  size_t runs = 0;

  multi.ForEach(&args, [&runs](const commands::Command*) {
    ++runs;
    return runs < 2;
  });

  EXPECT_EQ(runs, 2U);
  EXPECT_EQ(args[1], "key:1");
}

TEST(MultiStateTest, ResetEndsTheTransactionAndDropsTheQueue) {
  MultiState multi;
  multi.Begin();
  QueueSets(3, "v", &multi);
  multi.MarkRejected();

  multi.Reset();

  EXPECT_FALSE(multi.Active());
  EXPECT_FALSE(multi.Failed());
  EXPECT_EQ(multi.Size(), 0U);
  EXPECT_TRUE(Queued(multi).empty());
}

TEST(MultiStateTest, ResetKeepsASmallQueueForTheNextTransaction) {
  MultiState multi;
  multi.Begin();
  QueueSets(10, "value", &multi);
  const size_t allocated = multi.AllocatedBytes();
  ASSERT_GT(allocated, 0U);

  multi.Reset();

  EXPECT_EQ(multi.AllocatedBytes(), allocated);
}

TEST(MultiStateTest, ResetReleasesAQueueOfManySmallCommands) {
  MultiState multi;
  const size_t allocated_when_empty = multi.AllocatedBytes();
  multi.Begin();
  // Under 64 KiB of arguments, but more than that with the per-command and
  // per-argument entries.
  QueueSets(10'000, "", &multi);
  ASSERT_GT(multi.AllocatedBytes(), size_t{64} * 1024);

  multi.Reset();

  EXPECT_EQ(multi.AllocatedBytes(), allocated_when_empty);
}

TEST(MultiStateTest, ResetReleasesAQueueOfLargeArguments) {
  MultiState multi;
  const size_t allocated_when_empty = multi.AllocatedBytes();
  multi.Begin();
  QueueSets(2, std::string(size_t{64} * 1024, 'v'), &multi);

  multi.Reset();

  EXPECT_EQ(multi.AllocatedBytes(), allocated_when_empty);
}

TEST(MultiStateTest, QueueGrowsItsBuffersGeometrically) {
  MultiState multi;
  multi.Begin();
  size_t growths = 0;
  size_t allocated = multi.AllocatedBytes();
  const commands::Command* const set_command = commands::Find("SET");
  for (size_t i = 0; i < 10'000; ++i) {
    ASSERT_TRUE(multi.Queue(set_command, {"SET", "key", "value"}));
    if (multi.AllocatedBytes() != allocated) {
      allocated = multi.AllocatedBytes();
      ++growths;
    }
  }

  // Three buffers that each double: far fewer growths than commands.
  EXPECT_LT(growths, 100U);
}

TEST(MultiStateTest, AReleasedQueueIsUsableAgain) {
  MultiState multi;
  multi.Begin();
  QueueSets(10'000, "value", &multi);
  multi.Reset();
  multi.Begin();
  QueueSets(2, "again", &multi);

  const auto queued = Queued(multi);

  ASSERT_EQ(queued.size(), 2U);
  EXPECT_EQ(queued[1].second, (QueuedArgs{"SET", "key:1", "again"}));
}

TEST(MultiStateTest, HasWriteCommandReportsQueuedWrites) {
  MultiState multi;
  multi.Begin();
  ASSERT_TRUE(multi.Queue(commands::Find("GET"), {"GET", "key"}));
  EXPECT_FALSE(multi.HasWriteCommand());
  ASSERT_TRUE(multi.Queue(commands::Find("SET"), {"SET", "key", "value"}));
  EXPECT_TRUE(multi.HasWriteCommand());
}
}  // namespace
}  // namespace redis_simple
