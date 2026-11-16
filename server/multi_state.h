#ifndef REDIS_SIMPLE_SERVER_MULTI_STATE_H_
#define REDIS_SIMPLE_SERVER_MULTI_STATE_H_

#include <cstddef>
#include <cstdint>
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
  bool Queue(const commands::Command* command,
             const commands::CommandArgs& args);
  size_t Size() const { return commands_.size(); }
  bool HasWriteCommand() const;
  // Calls `run(command)` for each queued command in order, with `*args` set
  // to its arguments, until `run` returns false.
  template <typename Run>
  void ForEach(commands::CommandArgs* const args, const Run& run) const {
    size_t first_arg = 0;
    size_t arg_start = 0;
    for (const QueuedCommand& queued : commands_) {
      args->clear();
      for (size_t index = first_arg; index < queued.arg_end; ++index) {
        const size_t arg_end = arg_ends_[index];
        args->emplace_back(args_.data() + arg_start, arg_end - arg_start);
        arg_start = arg_end;
      }
      first_arg = queued.arg_end;
      if (!run(queued.command)) {
        return;
      }
    }
  }
  // Ends the transaction and drops the queue. A queue that grew past a small
  // size is released, so one large transaction does not stay allocated for
  // the rest of the connection.
  void Reset();
  // Bytes allocated for the queue, used or not.
  size_t AllocatedBytes() const {
    return args_.capacity() + arg_ends_.capacity() * sizeof(ArgEnd) +
           commands_.capacity() * sizeof(QueuedCommand);
  }

 private:
  // An offset into `args_`, which the queue limit keeps under 4 GiB.
  using ArgEnd = uint32_t;
  // A queued command's arguments are the entries of `arg_ends_` from the
  // previous command's `arg_end` up to its own; each is where an argument
  // ends in `args_`, and the next one starts there.
  struct QueuedCommand {
    const commands::Command* command;
    size_t arg_end;
  };

  bool active_{};
  bool failed_{};
  std::vector<QueuedCommand> commands_;
  std::vector<ArgEnd> arg_ends_;
  std::string args_;
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_MULTI_STATE_H_
