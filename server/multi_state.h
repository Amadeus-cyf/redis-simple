#ifndef REDIS_SIMPLE_SERVER_MULTI_STATE_H_
#define REDIS_SIMPLE_SERVER_MULTI_STATE_H_

#include <cstddef>
#include <string>
#include <vector>

#include "server/commands/command.h"

namespace redis_simple {
// A client's open MULTI transaction: the commands queued for EXEC (Redis'
// multiState). Queued arguments view the query buffer, which is reused after
// each read, so the queue copies them into one contiguous buffer.
class MultiState {
 public:
  bool Active() const { return active_; }
  void Begin() { active_ = true; }
  // Makes EXEC abort: a command was rejected while the transaction was open.
  void MarkRejected() { failed_ = failed_ || active_; }
  bool Failed() const { return failed_; }
  // Copies a command into the queue. When the queued arguments would pass
  // the size limit, it releases the queue and marks the transaction failed
  // instead, and returns false.
  bool Queue(const command::Command* command, const command::CommandArgs& args);
  size_t Size() const { return commands_.size(); }
  bool HasWriteCommand() const;
  // Calls `run(command)` for each queued command in order, with `*args` set
  // to its arguments, until `run` returns false.
  template <typename Run>
  void ForEach(command::CommandArgs* const args, const Run& run) const {
    for (const QueuedCommand& queued : commands_) {
      args->clear();
      const size_t first_arg = queued.arg_end - queued.arg_count;
      size_t arg_start = first_arg == 0 ? 0 : arg_ends_[first_arg - 1];
      for (size_t index = first_arg; index < queued.arg_end; ++index) {
        const size_t arg_end = arg_ends_[index];
        args->emplace_back(args_.data() + arg_start, arg_end - arg_start);
        arg_start = arg_end;
      }
      if (!run(queued.command)) {
        return;
      }
    }
  }
  // Ends the transaction and drops the queue.
  void Reset();

 private:
  // A queued command's arguments are the `arg_count` entries of `arg_ends_`
  // before `arg_end`, which index into `args_`.
  struct QueuedCommand {
    const command::Command* command;
    size_t arg_end;
    size_t arg_count;
  };

  bool active_{};
  bool failed_{};
  std::vector<QueuedCommand> commands_;
  std::vector<size_t> arg_ends_;
  std::string args_;
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_MULTI_STATE_H_
