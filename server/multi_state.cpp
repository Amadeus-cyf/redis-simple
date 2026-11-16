#include "server/multi_state.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "server/commands/command.h"

namespace redis_simple {
namespace {
// Upper bound for the argument bytes one MULTI may queue: Redis' default
// client-query-buffer-limit.
constexpr size_t kMaxQueuedBytes = size_t{1024} * 1024 * 1024;
// A queue that allocated more than this is released after its transaction
// ends; a smaller one is kept for the client's next transaction.
constexpr size_t kRetainedBytes = size_t{64} * 1024;
}  // namespace

bool MultiState::Queue(const commands::Command* const command,
                       const commands::CommandArgs& args) {
  static_assert(kMaxQueuedBytes <= std::numeric_limits<ArgEnd>::max());
  size_t bytes = 0;
  for (const std::string_view argument : args) {
    bytes += argument.size();
  }
  if (bytes > kMaxQueuedBytes - args_.size()) {
    Reset();
    active_ = true;
    failed_ = true;
    return false;
  }
  // No reserve of the exact size here: it would reallocate, and copy every
  // argument queued so far, on each command.
  for (const std::string_view argument : args) {
    args_.append(argument);
    arg_ends_.push_back(static_cast<ArgEnd>(args_.size()));
  }
  commands_.push_back({command, arg_ends_.size()});
  return true;
}

bool MultiState::HasWriteCommand() const {
  return std::ranges::any_of(commands_, [](const QueuedCommand& queued) {
    return queued.command->access == commands::CommandAccess::kWrite;
  });
}

void MultiState::Reset() {
  active_ = false;
  failed_ = false;
  if (AllocatedBytes() > kRetainedBytes) {
    // Assigning empty containers frees the storage, which shrink_to_fit is
    // only asked to do.
    commands_ = std::vector<QueuedCommand>();
    arg_ends_ = std::vector<ArgEnd>();
    args_ = std::string();
    return;
  }
  commands_.clear();
  arg_ends_.clear();
  args_.clear();
}
}  // namespace redis_simple
