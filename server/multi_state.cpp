#include "server/multi_state.h"

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "server/commands/command.h"

namespace redis_simple {
namespace {
// Upper bound for the argument bytes one MULTI may queue: Redis' default
// client-query-buffer-limit.
constexpr size_t kMaxQueuedBytes = size_t{1024} * 1024 * 1024;
// Queue buffers larger than this are released after a transaction ends.
constexpr size_t kRetainedBytes = size_t{64} * 1024;
}  // namespace

bool MultiState::Queue(const commands::Command* const command,
                       const commands::CommandArgs& args) {
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
  args_.reserve(args_.size() + bytes);
  for (const std::string_view argument : args) {
    args_.append(argument);
    arg_ends_.push_back(args_.size());
  }
  commands_.push_back({command, arg_ends_.size(), args.size()});
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
  commands_.clear();
  arg_ends_.clear();
  args_.clear();
  if (args_.capacity() > kRetainedBytes) {
    args_.shrink_to_fit();
    arg_ends_.shrink_to_fit();
    commands_.shrink_to_fit();
  }
}
}  // namespace redis_simple
