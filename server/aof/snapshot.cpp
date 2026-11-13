#include "server/aof/snapshot.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "server/aof/file_ops.h"
#include "server/aof/records.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/reply.h"
#include "utils/floats.h"

namespace redis_simple::aof {
namespace {
// Items per batched command, at most.
constexpr size_t kSnapshotBatchSize = 64;
// How much to write between syncs with incremental fsync (Redis'
// REDIS_AUTOSYNC_BYTES).
constexpr size_t kAutoSyncBytes = size_t{4} * 1024 * 1024;

// Collects a collection's items into commands such as "SADD key m1 m2 ...",
// starting a new command after kSnapshotBatchSize items or about
// `snapshot_block_bytes`, and hands each finished command to `sink`.
template <typename Sink>
class SnapshotBatch {
 public:
  SnapshotBatch(std::string_view command, std::string_view key,
                size_t elements_per_item, const Limits& limits, Sink* sink)
      : command_(command),
        key_(key),
        elements_per_item_(elements_per_item),
        limits_(&limits),
        sink_(sink) {}

  bool Append(std::initializer_list<std::string_view> elements) {
    if (elements.size() != elements_per_item_) {
      return false;
    }
    size_t item_size = 0;
    for (const auto element : elements) {
      const size_t encoded_size = RespBulkSize(element);
      if (encoded_size == std::numeric_limits<size_t>::max() ||
          !AddSize(encoded_size, &item_size)) {
        return false;
      }
    }

    if (item_count_ > 0 &&
        (item_count_ == kSnapshotBatchSize ||
         body_.size() >
             limits_->snapshot_block_bytes -
                 std::min(item_size, limits_->snapshot_block_bytes))) {
      if (!Flush()) {
        return false;
      }
    }

    for (const auto element : elements) {
      reply::AppendBulkString(element, &body_);
    }
    ++item_count_;
    return CommandSize() <= limits_->max_replay_command_bytes;
  }

  bool Finish() { return Flush(); }

 private:
  [[nodiscard]] size_t CommandSize() const {
    const size_t element_count = 2 + (item_count_ * elements_per_item_);
    size_t size = RespArrayHeaderSize(element_count);
    if (!AddSize(RespBulkSize(command_), &size) ||
        !AddSize(RespBulkSize(key_), &size) || !AddSize(body_.size(), &size)) {
      return std::numeric_limits<size_t>::max();
    }
    return size;
  }

  bool Flush() {
    if (item_count_ == 0) {
      return true;
    }
    const size_t element_count = 2 + (item_count_ * elements_per_item_);
    std::string header = reply::FromArrayHeader(element_count);
    reply::AppendBulkString(command_, &header);
    reply::AppendBulkString(key_, &header);
    if (body_.size() > limits_->max_replay_command_bytes ||
        header.size() > limits_->max_replay_command_bytes - body_.size() ||
        !(*sink_)(std::move(header)) || !(*sink_)(std::move(body_))) {
      return false;
    }
    body_.clear();
    item_count_ = 0;
    return true;
  }

  std::string_view command_;
  std::string_view key_;
  size_t elements_per_item_;
  const Limits* limits_;
  Sink* sink_;
  std::string body_;
  size_t item_count_{};
};

template <typename Sink>
bool EmitFixedCommand(std::initializer_list<std::string_view> elements,
                      const Limits& limits, Sink* sink) {
  std::string record;
  if (!AppendFixedCommand(elements, &record) ||
      record.size() > limits.max_replay_command_bytes) {
    return false;
  }
  return (*sink)(std::move(record));
}

// A string as one SET, or, when it is larger than a block, an empty SET
// followed by APPENDs of block-sized pieces.
template <typename Sink>
bool EmitString(std::string_view key, std::string_view value,
                const Limits& limits, Sink* sink) {
  size_t fixed_overhead = RespBulkSize("APPEND");
  if (!AddSize(RespBulkSize(key), &fixed_overhead) ||
      !AddSize(64, &fixed_overhead)) {
    return false;
  }
  if (fixed_overhead >= limits.max_replay_command_bytes) {
    return false;
  }
  const size_t target =
      std::min(limits.snapshot_block_bytes, limits.max_replay_command_bytes);
  const size_t chunk_size = std::max<size_t>(
      1, target > fixed_overhead
             ? target - fixed_overhead
             : limits.max_replay_command_bytes - fixed_overhead);
  if (value.size() <= chunk_size) {
    return EmitFixedCommand({"SET", key, value}, limits, sink);
  }
  if (!EmitFixedCommand({"SET", key, ""}, limits, sink)) {
    return false;
  }
  for (size_t offset = 0; offset < value.size(); offset += chunk_size) {
    const size_t size = std::min(chunk_size, value.size() - offset);
    if (!EmitFixedCommand({"APPEND", key, value.substr(offset, size)}, limits,
                          sink)) {
      return false;
    }
  }
  return true;
}

template <typename Sink>
bool EmitKey(std::string_view key, const db::RedisObject& object,
             std::optional<int64_t> expire, const Limits& limits, Sink* sink) {
  bool encoded = false;
  switch (object.Type()) {
    case db::RedisObject::ObjectType::kString:
      encoded = EmitString(key, object.String(), limits, sink);
      break;
    case db::RedisObject::ObjectType::kSet: {
      const data_types::Set* set = object.Set();
      SnapshotBatch batch("SADD", key, 1, limits, sink);
      encoded = set->ForEachMember([&batch](std::string_view member) {
        return batch.Append({member});
      }) && batch.Finish();
      break;
    }
    case db::RedisObject::ObjectType::kList: {
      const data_types::List* list = object.List();
      const size_t size = list->Size();
      if (size == 0) {
        encoded = true;
        break;
      }
      SnapshotBatch batch("RPUSH", key, 1, limits, sink);
      encoded = list->ForEach(0, size - 1, [&batch](std::string_view value) {
        return batch.Append({value});
      }) && batch.Finish();
      break;
    }
    case db::RedisObject::ObjectType::kZSet: {
      const data_types::ZSet* zset = object.ZSet();
      SnapshotBatch batch("ZADD", key, 2, limits, sink);
      encoded = zset->ForEachEntry([&batch](std::string_view member,
                                            double score) {
        const std::string score_text = utils::FloatToString(score);
        return batch.Append({score_text, member});
      }) && batch.Finish();
      break;
    }
    case db::RedisObject::ObjectType::kHash: {
      const data_types::Hash* hash = object.Hash();
      SnapshotBatch batch("HSET", key, 2, limits, sink);
      encoded = hash->ForEachEntry([&batch](std::string_view field,
                                            std::string_view value) {
        return batch.Append({field, value});
      }) && batch.Finish();
      break;
    }
  }
  if (!encoded) {
    return false;
  }
  if (expire.has_value()) {
    std::string record;
    if (!AppendExpireAt(key, *expire, &record) ||
        record.size() > limits.max_replay_command_bytes ||
        !(*sink)(std::move(record))) {
      return false;
    }
  }
  return true;
}
}  // namespace

SnapshotStatus WriteSnapshotFile(db::RedisDb* const db, const std::string& path,
                                 const SnapshotOptions& options,
                                 FileOps* const file_ops) {
  const int fd = OpenFile(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return SnapshotStatus::kWriteFailed;
  }
  SnapshotStatus status = SnapshotStatus::kWritten;
  size_t unsynced_bytes = 0;
  const auto write_block = [&options, file_ops, fd, &status,
                            &unsynced_bytes](std::string& block) {
    std::array<std::string, 1> blocks{std::move(block)};
    block.clear();
    if (!WriteBlocks(file_ops, fd, blocks)) {
      status = SnapshotStatus::kWriteFailed;
      return false;
    }
    unsynced_bytes += blocks[0].size();
    if (options.incremental_fsync && unsynced_bytes >= kAutoSyncBytes) {
      unsynced_bytes = 0;
      if (!SyncFile(file_ops, fd)) {
        status = SnapshotStatus::kSyncFailed;
        return false;
      }
    }
    return true;
  };
  std::string block;
  if (options.timestamp) {
    AppendTimestamp(UnixSeconds(), &block);
  }
  if (db->KeyCount() > 0) {
    block.append(kSelectRecord);
  }
  // Records collect in `block`, which is written whenever it fills.
  auto sink = [&options, &block, &write_block](std::string record) {
    if (block.empty()) {
      block = std::move(record);
    } else {
      block.append(record);
    }
    return block.size() < options.limits.snapshot_block_bytes ||
           write_block(block);
  };
  const bool encoded =
      db->ForEachObject([db, &options, &sink](std::string_view key,
                                              const db::RedisObject& object) {
        return EmitKey(key, object, db->Expiration(key), options.limits, &sink);
      });
  if (!encoded) {
    if (status == SnapshotStatus::kWritten) {
      status = SnapshotStatus::kEncodeFailed;
    }
  } else if (write_block(block) && !SyncFile(file_ops, fd)) {
    status = SnapshotStatus::kSyncFailed;
  }
  if (close(fd) < 0 && status == SnapshotStatus::kWritten) {
    status = SnapshotStatus::kWriteFailed;
  }
  if (status != SnapshotStatus::kWritten) {
    unlink(path.c_str());
  }
  return status;
}
}  // namespace redis_simple::aof
