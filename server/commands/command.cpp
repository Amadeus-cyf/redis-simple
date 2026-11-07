#include "server/commands/command.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "server/commands/handlers.h"

namespace redis_simple::command {
namespace {
constexpr auto kUnlimited = std::numeric_limits<size_t>::max();

constexpr CommandArity FixedArity(size_t count) { return {count, count}; }

constexpr CommandArity VariableArity(size_t minimum) {
  return {minimum, kUnlimited};
}

constexpr KeySpec NoKeys() { return {}; }

constexpr KeySpec OneKey() { return {0, 0, 1}; }

constexpr KeySpec AllKeys(size_t first = 0, size_t step = 1) {
  return {first, KeySpec::kAllRemaining, step};
}

constexpr Command ReadCommand(std::string_view name, CommandCallback callback,
                              CommandArity arity, KeySpec keys = NoKeys()) {
  return {name, callback, arity, CommandAccess::kReadOnly, keys};
}

constexpr Command WriteCommand(std::string_view name, CommandCallback callback,
                               CommandArity arity, KeySpec keys = NoKeys()) {
  return {name, callback, arity, CommandAccess::kWrite, keys};
}

constexpr Command AdminCommand(std::string_view name, CommandCallback callback,
                               CommandArity arity) {
  return {name, callback, arity, CommandAccess::kAdmin, NoKeys()};
}

constexpr Command ConnectionCommand(std::string_view name,
                                    CommandCallback callback,
                                    CommandArity arity) {
  return {name, callback, arity, CommandAccess::kConnection, NoKeys()};
}

constexpr Command AllowBeforeAuth(Command command) {
  command.allowed_before_auth = true;
  return command;
}

constexpr Command RunsImmediatelyInMulti(Command command) {
  command.queued_in_multi = false;
  return command;
}

constexpr char FoldAsciiCase(char value) {
  return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A'))
                                      : value;
}

constexpr int CompareIgnoreCase(std::string_view left, std::string_view right) {
  const size_t length = left.size() < right.size() ? left.size() : right.size();
  for (size_t index = 0; index < length; ++index) {
    const char left_char = FoldAsciiCase(left[index]);
    const char right_char = FoldAsciiCase(right[index]);
    if (left_char != right_char) {
      return left_char < right_char ? -1 : 1;
    }
  }
  if (left.size() == right.size()) {
    return 0;
  }
  return left.size() < right.size() ? -1 : 1;
}

constexpr std::array kCommandTable = {
    WriteCommand("APPEND", strings::HandleAppend, FixedArity(2), OneKey()),
    AllowBeforeAuth(
        ConnectionCommand("AUTH", session::HandleAuth, VariableArity(1))),
    AdminCommand("BGREWRITEAOF", persistence::HandleBgRewriteAof,
                 FixedArity(0)),
    ConnectionCommand("CLIENT", session::HandleClient, VariableArity(1)),
    ReadCommand("DBSIZE", key::HandleDbSize, FixedArity(0)),
    WriteCommand("DECR", strings::HandleDecr, FixedArity(1), OneKey()),
    WriteCommand("DECRBY", strings::HandleDecrBy, FixedArity(2), OneKey()),
    WriteCommand("DEL", key::HandleDel, VariableArity(1), AllKeys()),
    RunsImmediatelyInMulti(ConnectionCommand(
        "DISCARD", transaction::HandleDiscard, FixedArity(0))),
    ConnectionCommand("ECHO", session::HandleEcho, FixedArity(1)),
    RunsImmediatelyInMulti(
        ConnectionCommand("EXEC", transaction::HandleExec, FixedArity(0))),
    ReadCommand("EXISTS", key::HandleExists, VariableArity(1), AllKeys()),
    WriteCommand("EXPIRE", key::HandleExpire, VariableArity(2), OneKey()),
    WriteCommand("EXPIREAT", key::HandleExpireAt, VariableArity(2), OneKey()),
    WriteCommand("FLUSHDB", key::HandleFlushDb, VariableArity(0)),
    ReadCommand("GET", strings::HandleGet, FixedArity(1), OneKey()),
    WriteCommand("HDEL", hashes::HandleHDel, VariableArity(2), OneKey()),
    AllowBeforeAuth(
        ConnectionCommand("HELLO", session::HandleHello, VariableArity(0))),
    ReadCommand("HEXISTS", hashes::HandleHExists, FixedArity(2), OneKey()),
    ReadCommand("HGET", hashes::HandleHGet, FixedArity(2), OneKey()),
    ReadCommand("HGETALL", hashes::HandleHGetAll, FixedArity(1), OneKey()),
    WriteCommand("HINCRBY", hashes::HandleHIncrBy, FixedArity(3), OneKey()),
    ReadCommand("HKEYS", hashes::HandleHKeys, FixedArity(1), OneKey()),
    ReadCommand("HLEN", hashes::HandleHLen, FixedArity(1), OneKey()),
    ReadCommand("HMGET", hashes::HandleHMGet, VariableArity(2), OneKey()),
    WriteCommand("HMSET", hashes::HandleHMSet, VariableArity(3), OneKey()),
    WriteCommand("HSET", hashes::HandleHSet, VariableArity(3), OneKey()),
    ReadCommand("HVALS", hashes::HandleHVals, FixedArity(1), OneKey()),
    WriteCommand("INCR", strings::HandleIncr, FixedArity(1), OneKey()),
    WriteCommand("INCRBY", strings::HandleIncrBy, FixedArity(2), OneKey()),
    AdminCommand("INFO", persistence::HandleInfo, {0, 1}),
    ReadCommand("LINDEX", lists::HandleLIndex, FixedArity(2), OneKey()),
    ReadCommand("LLEN", lists::HandleLLen, FixedArity(1), OneKey()),
    WriteCommand("LPOP", lists::HandleLPop, {1, 2}, OneKey()),
    WriteCommand("LPUSH", lists::HandleLPush, VariableArity(2), OneKey()),
    ReadCommand("LRANGE", lists::HandleLRange, FixedArity(3), OneKey()),
    WriteCommand("LREM", lists::HandleLRem, FixedArity(3), OneKey()),
    WriteCommand("LSET", lists::HandleLSet, FixedArity(3), OneKey()),
    WriteCommand("LTRIM", lists::HandleLTrim, FixedArity(3), OneKey()),
    ReadCommand("MGET", strings::HandleMGet, VariableArity(1), AllKeys()),
    WriteCommand("MSET", strings::HandleMSet, VariableArity(2), AllKeys(0, 2)),
    RunsImmediatelyInMulti(
        ConnectionCommand("MULTI", transaction::HandleMulti, FixedArity(0))),
    WriteCommand("PERSIST", key::HandlePersist, FixedArity(1), OneKey()),
    WriteCommand("PEXPIRE", key::HandlePExpire, VariableArity(2), OneKey()),
    WriteCommand("PEXPIREAT", key::HandlePExpireAt, VariableArity(2), OneKey()),
    ConnectionCommand("PING", session::HandlePing, {0, 1}),
    ReadCommand("PTTL", key::HandlePTtl, FixedArity(1), OneKey()),
    AllowBeforeAuth(RunsImmediatelyInMulti(
        ConnectionCommand("QUIT", session::HandleQuit, FixedArity(0)))),
    WriteCommand("RENAME", key::HandleRename, FixedArity(2), {0, 1, 1}),
    WriteCommand("RPOP", lists::HandleRPop, {1, 2}, OneKey()),
    WriteCommand("RPUSH", lists::HandleRPush, VariableArity(2), OneKey()),
    WriteCommand("SADD", sets::HandleSAdd, VariableArity(2), OneKey()),
    ReadCommand("SCAN", key::HandleScan, VariableArity(1)),
    ReadCommand("SCARD", sets::HandleSCard, FixedArity(1), OneKey()),
    ReadCommand("SDIFF", sets::HandleSDiff, VariableArity(1), AllKeys()),
    ConnectionCommand("SELECT", session::HandleSelect, FixedArity(1)),
    WriteCommand("SET", strings::HandleSet, VariableArity(2), OneKey()),
    ReadCommand("SINTER", sets::HandleSInter, VariableArity(1), AllKeys()),
    ReadCommand("SISMEMBER", sets::HandleSIsMember, FixedArity(2), OneKey()),
    ReadCommand("SMEMBERS", sets::HandleSMembers, FixedArity(1), OneKey()),
    WriteCommand("SREM", sets::HandleSRem, VariableArity(2), OneKey()),
    ReadCommand("STRLEN", strings::HandleStrLen, FixedArity(1), OneKey()),
    ReadCommand("SUNION", sets::HandleSUnion, VariableArity(1), AllKeys()),
    ReadCommand("TTL", key::HandleTtl, FixedArity(1), OneKey()),
    ReadCommand("TYPE", key::HandleType, FixedArity(1), OneKey()),
    WriteCommand("UNLINK", key::HandleUnlink, VariableArity(1), AllKeys()),
    ConnectionCommand("UNWATCH", transaction::HandleUnwatch, FixedArity(0)),
    RunsImmediatelyInMulti(
        ConnectionCommand("WATCH", transaction::HandleWatch, VariableArity(1))),
    WriteCommand("ZADD", zsets::HandleZAdd, VariableArity(3), OneKey()),
    ReadCommand("ZCARD", zsets::HandleZCard, FixedArity(1), OneKey()),
    ReadCommand("ZCOUNT", zsets::HandleZCount, FixedArity(3), OneKey()),
    ReadCommand("ZLEXCOUNT", zsets::HandleZLexCount, FixedArity(3), OneKey()),
    ReadCommand("ZRANGE", zsets::HandleZRange, VariableArity(3), OneKey()),
    ReadCommand("ZRANGEBYLEX", zsets::HandleZRangeByLex, VariableArity(3),
                OneKey()),
    ReadCommand("ZRANGEBYSCORE", zsets::HandleZRangeByScore, VariableArity(3),
                OneKey()),
    ReadCommand("ZRANK", zsets::HandleZRank, {2, 3}, OneKey()),
    WriteCommand("ZREM", zsets::HandleZRem, VariableArity(2), OneKey()),
    ReadCommand("ZREVRANGE", zsets::HandleZRevRange, VariableArity(3),
                OneKey()),
    ReadCommand("ZREVRANGEBYLEX", zsets::HandleZRevRangeByLex, VariableArity(3),
                OneKey()),
    ReadCommand("ZREVRANGEBYSCORE", zsets::HandleZRevRangeByScore,
                VariableArity(3), OneKey()),
    ReadCommand("ZREVRANK", zsets::HandleZRevRank, {2, 3}, OneKey()),
    ReadCommand("ZSCORE", zsets::HandleZScore, FixedArity(2), OneKey()),
};

constexpr bool CommandTableIsSorted() {
  for (size_t index = 1; index < kCommandTable.size(); ++index) {
    if (CompareIgnoreCase(kCommandTable[index - 1].name,
                          kCommandTable[index].name) >= 0) {
      return false;
    }
  }
  return true;
}

static_assert(CommandTableIsSorted(), "command table must remain sorted");

// Find() hashes the requested name in one pass and probes a compile-time
// open-addressing index over the table, usually with a single comparison.
// Table names are upper-case letters only, so setting bit 5 (`| 0x20`) folds
// case without branches: for a letter L, `x | 0x20 == L | 0x20` holds only
// when x is L or its lower-case form.
constexpr size_t kMaxNameLength = 16;
constexpr size_t kIndexSize = 256;
constexpr size_t kIndexMask = kIndexSize - 1;
static_assert(std::has_single_bit(kIndexSize));
static_assert(kCommandTable.size() * 2 <= kIndexSize,
              "grow kIndexSize to keep the command index sparse");

constexpr uint32_t kFnvOffsetBasis = 2166136261U;
constexpr uint32_t kFnvPrime = 16777619U;

constexpr unsigned char kCaseBit = 0x20;

constexpr unsigned char FoldForLookup(char value) {
  return static_cast<unsigned char>(value) | kCaseBit;
}

// Case-insensitive comparison against a table name, which is upper-case
// letters only.
constexpr bool EqualsCommandName(std::string_view name,
                                 std::string_view table_name) {
  if (name.size() != table_name.size()) {
    return false;
  }
  unsigned char difference = 0;
  for (size_t position = 0; position < name.size(); ++position) {
    difference |=
        FoldForLookup(name[position]) ^ FoldForLookup(table_name[position]);
  }
  return difference == 0;
}

constexpr uint32_t HashStep(uint32_t hash, char value) {
  return (hash ^ FoldForLookup(value)) * kFnvPrime;
}

constexpr bool CommandNamesAreIndexable() {
  for (const Command& command : kCommandTable) {
    if (command.name.empty() || command.name.size() > kMaxNameLength) {
      return false;
    }
    for (const char value : command.name) {
      if (value < 'A' || value > 'Z') {
        return false;
      }
    }
  }
  return true;
}

static_assert(CommandNamesAreIndexable(),
              "command names must be upper-case letters, at most "
              "kMaxNameLength of them");

// Each slot holds a table index plus one; zero marks an empty slot.
constexpr std::array<uint8_t, kIndexSize> BuildIndex() {
  std::array<uint8_t, kIndexSize> index{};
  for (size_t position = 0; position < kCommandTable.size(); ++position) {
    uint32_t hash = kFnvOffsetBasis;
    for (const char value : kCommandTable[position].name) {
      hash = HashStep(hash, value);
    }
    size_t slot = hash & kIndexMask;
    while (index[slot] != 0) {
      slot = (slot + 1) & kIndexMask;
    }
    index[slot] = static_cast<uint8_t>(position + 1);
  }
  return index;
}

constexpr const std::array<uint8_t, kIndexSize> kCommandIndex = BuildIndex();
}  // namespace

const Command* Find(std::string_view name) {
  if (name.empty() || name.size() > kMaxNameLength) {
    return nullptr;
  }
  uint32_t hash = kFnvOffsetBasis;
  for (const char value : name) {
    hash = HashStep(hash, value);
  }
  for (size_t slot = hash & kIndexMask;; slot = (slot + 1) & kIndexMask) {
    const uint8_t entry = kCommandIndex[slot];
    if (entry == 0) {
      return nullptr;
    }
    const Command& command = kCommandTable[entry - 1];
    if (EqualsCommandName(name, command.name)) {
      return &command;
    }
  }
}
}  // namespace redis_simple::command
