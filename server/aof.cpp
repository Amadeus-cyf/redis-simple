#include "server/aof.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "logging/logger.h"
#include "memory/dynamic_buffer.h"
#include "server/client.h"
#include "server/commands/command.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "server/request_parser.h"
#include "utils/check.h"
#include "utils/float_utils.h"

namespace redis_simple::aof {
namespace {
constexpr size_t kReadChunkSize = size_t{64} * 1024;
// Redis' AOF_ANNOTATION_LINE_MAX_LEN.
constexpr size_t kMaxAnnotationBytes = 1024;
constexpr size_t kSnapshotBatchSize = 64;
// Covers a bulk-string frame or array header, including a size_t decimal value.
constexpr size_t kRespElementOverhead = 32;
constexpr std::string_view kLongestInt64 = "-9223372036854775808";
// Redis' AOF timing: `everysec` syncs once a second, a write waits at most
// two seconds for a running sync, and write errors are logged at most every
// 30 seconds.
constexpr std::chrono::seconds kSyncInterval{1};
constexpr std::chrono::seconds kMaxFlushPostpone{2};
constexpr std::chrono::seconds kWriteErrorLogInterval{30};
// Redis' REDIS_AUTOSYNC_BYTES, for aof-rewrite-incremental-fsync.
constexpr size_t kAutoSyncBytes = size_t{4} * 1024 * 1024;
constexpr std::string_view kMultiRecord = "*1\r\n$5\r\nMULTI\r\n";

constexpr std::string_view kExecRecord = "*1\r\n$4\r\nEXEC\r\n";
// This server has one database. Redis selects it before the first command of
// each file and of a snapshot, and its AOFs load here only if they use no
// other.
constexpr std::string_view kSelectRecord = "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n";

class SystemFileOps final : public FileOps {
 public:
  ssize_t Write(int fd, const void* data, size_t size) override {
    return write(fd, data, size);
  }

  ssize_t WriteVector(int fd, const iovec* blocks, int count) override {
    return writev(fd, blocks, count);
  }

  int Sync(int fd) override { return fsync(fd); }

  int Rename(const char* source, const char* destination) override {
    return std::rename(source, destination);
  }
};

std::shared_ptr<FileOps> DefaultFileOps() {
  // SystemFileOps is stateless, so each Aof can own its own instance.
  return std::make_shared<SystemFileOps>();
}

size_t MaxIovCount() {
  static const size_t kMaxIovCount = [] {
    const int64_t system_limit = sysconf(_SC_IOV_MAX);
    const size_t limit =
        system_limit > 0 ? static_cast<size_t>(system_limit) : 1024;
    return std::min(limit,
                    static_cast<size_t>(std::numeric_limits<int>::max()));
  }();
  return kMaxIovCount;
}

bool SyncFile(FileOps* const file_ops, int fd) {
  while (file_ops->Sync(fd) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

// Exit codes of the rewrite child, which cannot log.
constexpr int kChildSucceeded = 0;
constexpr int kChildWriteFailed = 1;
constexpr int kChildSyncFailed = 2;
constexpr int kChildSnapshotFailed = 3;

std::optional<size_t> FileSize(int fd) {
  struct stat file_info{};
  if (fstat(fd, &file_info) < 0 || file_info.st_size < 0 ||
      static_cast<uintmax_t>(file_info.st_size) >
          std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<size_t>(file_info.st_size);
}

// Writes as much of `data` as the file takes, retrying short and interrupted
// writes. Returns the bytes written; on an error, sets `error_number`.
size_t WritePrefix(FileOps* const file_ops, int fd, std::string_view data,
                   int* const error_number) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t result =
        file_ops->Write(fd, data.data() + written, data.size() - written);
    if (result > 0) {
      written += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    *error_number = result < 0 ? errno : EIO;
    break;
  }
  return written;
}

// Redis' aofWrite: writes all of `data`, retrying short and interrupted
// writes. Returns the bytes written, or -1 if an error stopped it before the
// first byte.
ssize_t WriteAll(FileOps* const file_ops, int fd, std::string_view data) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t result =
        file_ops->Write(fd, data.data() + written, data.size() - written);
    if (result > 0) {
      written += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result == 0) {
      errno = ENOSPC;
    }
    return written > 0 ? static_cast<ssize_t>(written) : -1;
  }
  return static_cast<ssize_t>(written);
}

int64_t UnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Redis' "#TS:<unix time>" annotation, which loading skips.
void AppendTimestamp(int64_t seconds, std::string* const output) {
  output->append("#TS:").append(std::to_string(seconds)).append("\r\n");
}

// Writes every block with vectored writes, retrying short and interrupted
// writes.
template <typename Blocks>
bool WriteBlocks(FileOps* const file_ops, int fd, Blocks& blocks) {
  const size_t max_iov_count = MaxIovCount();
  std::vector<iovec> vectors;
  auto block = std::begin(blocks);
  while (block != std::end(blocks)) {
    vectors.clear();
    for (; block != std::end(blocks) && vectors.size() < max_iov_count;
         ++block) {
      if (!block->empty()) {
        vectors.push_back({block->data(), block->size()});
      }
    }
    size_t index = 0;
    while (index < vectors.size()) {
      ssize_t written = -1;
      while (true) {
        written =
            file_ops->WriteVector(fd, vectors.data() + index,
                                  static_cast<int>(vectors.size() - index));
        if (written >= 0 || errno != EINTR) {
          break;
        }
      }
      if (written <= 0) {
        return false;
      }
      auto consumed = static_cast<size_t>(written);
      while (consumed > 0) {
        iovec& vector = vectors[index];
        if (consumed < vector.iov_len) {
          vector.iov_base = static_cast<char*>(vector.iov_base) + consumed;
          vector.iov_len -= consumed;
          consumed = 0;
        } else {
          consumed -= vector.iov_len;
          ++index;
        }
      }
    }
  }
  return true;
}

// waitpid, retried when interrupted. Returns the child's pid, 0 if it is
// still running (with WNOHANG), or -1.
pid_t WaitForChild(pid_t child, int* const wait_status, int options) {
  while (true) {
    const pid_t result = waitpid(child, wait_status, options);
    if (result >= 0 || errno != EINTR) {
      return result;
    }
  }
}

// Multi-part AOF names, as in Redis 7.
constexpr std::string_view kBaseSuffix = ".base.aof";
constexpr std::string_view kIncrementSuffix = ".incr.aof";
constexpr std::string_view kManifestSuffix = ".manifest";
constexpr std::string_view kTempPrefix = "temp-";

// Redis' AOF_REWRITE_LIMITE_THRESHOLD and AOF_REWRITE_LIMITE_MAX_MINUTES.
constexpr size_t kFailuresBeforeBackoff = 3;
constexpr int64_t kMaxBackoffMinutes = 60;

char TypeCode(ManifestFile::Type type) {
  switch (type) {
    case ManifestFile::Type::kBase:
      return 'b';
    case ManifestFile::Type::kIncrement:
      return 'i';
    case ManifestFile::Type::kHistory:
      return 'h';
  }
  utils::Unreachable();
}

// Redis' pathIsBaseName: a name without a directory part.
bool ValidFileName(std::string_view name) {
  return !name.empty() && name.find_first_of("/\\") == std::string_view::npos;
}

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

bool IsPrint(char c) { return c >= 0x20 && c < 0x7f; }

std::optional<int> HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return std::nullopt;
}

// Redis' sdsneedsrepr and sdscatrepr: whether a manifest field must be
// quoted, and the quoted form.
bool NeedsQuoting(std::string_view value) {
  return std::ranges::any_of(value, [](char c) {
    return c == '\\' || c == '"' || !IsPrint(c) || IsSpace(c);
  });
}

void AppendQuoted(std::string_view value, std::string* const output) {
  output->push_back('"');
  for (const char c : value) {
    switch (c) {
      case '\\':
      case '"':
        output->push_back('\\');
        output->push_back(c);
        break;
      case '\n':
        output->append("\\n");
        break;
      case '\r':
        output->append("\\r");
        break;
      case '\t':
        output->append("\\t");
        break;
      case '\a':
        output->append("\\a");
        break;
      case '\b':
        output->append("\\b");
        break;
      default:
        if (IsPrint(c)) {
          output->push_back(c);
        } else {
          constexpr std::string_view kHex = "0123456789abcdef";
          const auto byte = static_cast<unsigned char>(c);
          output->append("\\x");
          output->push_back(kHex[byte >> 4]);
          output->push_back(kHex[byte & 0xf]);
        }
        break;
    }
  }
  output->push_back('"');
}

// Redis' sdssplitargs: splits a line into fields separated by whitespace,
// where a field may be "double quoted" with escapes or 'single quoted'.
// Returns nullopt for unbalanced quotes or a closing quote followed by
// anything but whitespace.
std::optional<std::vector<std::string>> SplitFields(std::string_view line) {
  std::vector<std::string> fields;
  size_t position = 0;
  const auto at = [&line](size_t index) {
    return index < line.size() ? line[index] : '\0';
  };
  while (true) {
    while (position < line.size() && IsSpace(line[position])) {
      ++position;
    }
    if (position >= line.size()) {
      return fields;
    }
    std::string field;
    bool in_double_quotes = false;
    bool in_single_quotes = false;
    bool done = false;
    while (!done) {
      const char c = at(position);
      if (in_double_quotes) {
        if (c == '\\' && at(position + 1) == 'x' &&
            HexDigit(at(position + 2)).has_value() &&
            HexDigit(at(position + 3)).has_value()) {
          field.push_back(
              static_cast<char>((HexDigit(at(position + 2)).value_or(0) * 16) +
                                HexDigit(at(position + 3)).value_or(0)));
          position += 3;
        } else if (c == '\\' && at(position + 1) != '\0') {
          ++position;
          switch (at(position)) {
            case 'n':
              field.push_back('\n');
              break;
            case 'r':
              field.push_back('\r');
              break;
            case 't':
              field.push_back('\t');
              break;
            case 'b':
              field.push_back('\b');
              break;
            case 'a':
              field.push_back('\a');
              break;
            default:
              field.push_back(at(position));
              break;
          }
        } else if (c == '"') {
          if (at(position + 1) != '\0' && !IsSpace(at(position + 1))) {
            return std::nullopt;
          }
          done = true;
        } else if (c == '\0') {
          return std::nullopt;
        } else {
          field.push_back(c);
        }
      } else if (in_single_quotes) {
        if (c == '\\' && at(position + 1) == '\'') {
          ++position;
          field.push_back('\'');
        } else if (c == '\'') {
          if (at(position + 1) != '\0' && !IsSpace(at(position + 1))) {
            return std::nullopt;
          }
          done = true;
        } else if (c == '\0') {
          return std::nullopt;
        } else {
          field.push_back(c);
        }
      } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\0') {
        done = true;
      } else if (c == '"') {
        in_double_quotes = true;
      } else if (c == '\'') {
        in_single_quotes = true;
      } else {
        field.push_back(c);
      }
      if (position < line.size()) {
        ++position;
      }
    }
    fields.push_back(std::move(field));
  }
}

// C's atoll, as Redis reads manifest numbers: leading whitespace and a sign,
// then digits up to the first other character; 0 if there are none.
int64_t ParseLeadingInteger(std::string_view text) {
  size_t position = 0;
  while (position < text.size() && IsSpace(text[position])) {
    ++position;
  }
  bool negative = false;
  if (position < text.size() &&
      (text[position] == '+' || text[position] == '-')) {
    negative = text[position] == '-';
    ++position;
  }
  uint64_t magnitude = 0;
  constexpr uint64_t kLimit = uint64_t{1} << 63;
  for (;
       position < text.size() && text[position] >= '0' && text[position] <= '9';
       ++position) {
    const auto digit = static_cast<uint64_t>(text[position] - '0');
    magnitude =
        magnitude > (kLimit - digit) / 10 ? kLimit : (magnitude * 10) + digit;
  }
  if (negative) {
    return magnitude >= kLimit ? std::numeric_limits<int64_t>::min()
                               : -static_cast<int64_t>(magnitude);
  }
  return magnitude >= kLimit ? std::numeric_limits<int64_t>::max()
                             : static_cast<int64_t>(magnitude);
}

bool EqualsIgnoringCase(std::string_view left, std::string_view right) {
  return std::ranges::equal(left, right, [](char a, char b) {
    const auto lower = [](char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return lower(a) == lower(b);
  });
}

std::optional<std::string> ReadWholeFile(const std::string& path) {
  int open_flags = O_RDONLY;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  const int fd = open(path.c_str(), open_flags);
  if (fd < 0) {
    return std::nullopt;
  }
  std::string contents;
  std::array<char, 4096> chunk{};
  while (true) {
    const ssize_t size = read(fd, chunk.data(), chunk.size());
    if (size > 0) {
      contents.append(chunk.data(), static_cast<size_t>(size));
      continue;
    }
    if (size < 0 && errno == EINTR) {
      continue;
    }
    close(fd);
    if (size < 0) {
      return std::nullopt;
    }
    return contents;
  }
}

ManifestFile MakeFile(std::string name, int64_t sequence,
                      ManifestFile::Type type) {
  ManifestFile file;
  file.name = std::move(name);
  file.sequence = sequence;
  file.type = type;
  return file;
}

bool PathExists(const std::string& path) {
  struct stat info{};
  return stat(path.c_str(), &info) == 0;
}

bool IsDirectory(const std::string& path) {
  struct stat info{};
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// The size of the file at `path`; nullopt, with errno set, if it is missing.
std::optional<size_t> PathSize(const std::string& path) {
  struct stat info{};
  if (stat(path.c_str(), &info) < 0) {
    return std::nullopt;
  }
  return info.st_size > 0 ? static_cast<size_t>(info.st_size) : 0;
}

AofError ChildError(int wait_status) {
  if (!WIFEXITED(wait_status)) {
    return AofError::kSnapshot;
  }
  switch (WEXITSTATUS(wait_status)) {
    case kChildWriteFailed:
      return AofError::kWrite;
    case kChildSyncFailed:
      return AofError::kSync;
    default:
      return AofError::kSnapshot;
  }
}

size_t RespBulkSize(std::string_view value) {
  constexpr size_t kFramingBytes = 5;
  size_t digits = 1;
  for (size_t size = value.size(); size >= 10; size /= 10) {
    ++digits;
  }
  if (value.size() >
      std::numeric_limits<size_t>::max() - digits - kFramingBytes) {
    return std::numeric_limits<size_t>::max();
  }
  return value.size() + digits + kFramingBytes;
}

size_t RespArrayHeaderSize(size_t element_count) {
  constexpr size_t kFramingBytes = 3;
  size_t digits = 1;
  for (size_t size = element_count; size >= 10; size /= 10) {
    ++digits;
  }
  return digits + kFramingBytes;
}

bool AddSize(size_t value, size_t* const total) {
  if (value > std::numeric_limits<size_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}

bool ReserveResp(size_t payload_size, size_t element_count,
                 size_t command_count, std::string* const output) {
  const size_t max_size = std::numeric_limits<size_t>::max();
  if (command_count > max_size - element_count) {
    return false;
  }
  const size_t overhead_count = element_count + command_count;
  if (overhead_count > max_size / kRespElementOverhead) {
    return false;
  }
  const size_t overhead = kRespElementOverhead * overhead_count;
  size_t reserve_size = output->size();
  if (!AddSize(payload_size, &reserve_size) ||
      !AddSize(overhead, &reserve_size) || reserve_size > output->max_size()) {
    return false;
  }
  if (reserve_size > output->capacity()) {
    const size_t capacity = output->capacity();
    const size_t grown_capacity =
        capacity <= output->max_size() / 2 ? capacity * 2 : output->max_size();
    output->reserve(std::max(reserve_size, grown_capacity));
  }
  return true;
}

bool ReserveCommands(std::initializer_list<std::string_view> elements,
                     size_t command_count, std::string* const output) {
  size_t payload_size = 0;
  for (const auto element : elements) {
    if (!AddSize(element.size(), &payload_size)) {
      return false;
    }
  }
  return ReserveResp(payload_size, elements.size(), command_count, output);
}

bool ReserveCommand(std::string_view command,
                    const std::vector<std::string_view>& args,
                    std::string* const output) {
  size_t payload_size = command.size();
  for (const auto arg : args) {
    if (!AddSize(arg.size(), &payload_size)) {
      return false;
    }
  }

  if (args.size() == std::numeric_limits<size_t>::max()) {
    return false;
  }
  return ReserveResp(payload_size, args.size() + 1, 1, output);
}

bool ReserveSetRecord(std::string_view key, std::string_view value,
                      bool has_expiration, std::string* const output) {
  if (!has_expiration) {
    return ReserveCommands({"SET", key, value}, 1, output);
  }
  return ReserveCommands({"SET", key, value, "PEXPIREAT", key, kLongestInt64},
                         2, output);
}

bool AppendCommand(std::string_view command,
                   const std::vector<std::string_view>& args,
                   std::string* const output) {
  if (!ReserveCommand(command, args, output)) {
    return false;
  }
  reply::AppendArrayHeader(args.size() + 1, output);
  reply::AppendBulkString(command, output);
  for (std::string_view arg : args) {
    reply::AppendBulkString(arg, output);
  }
  return true;
}

bool AppendFixedCommand(std::initializer_list<std::string_view> elements,
                        std::string* const output) {
  if (elements.size() == 0 || !ReserveCommands(elements, 1, output)) {
    return false;
  }
  reply::AppendArrayHeader(elements.size(), output);
  for (const auto element : elements) {
    reply::AppendBulkString(element, output);
  }
  return true;
}

bool AppendSingleKeyCommand(std::string_view command, std::string_view key,
                            std::string* const output) {
  if (!ReserveCommands({command, key}, 1, output)) {
    return false;
  }
  reply::AppendArrayHeader(2, output);
  reply::AppendBulkString(command, output);
  reply::AppendBulkString(key, output);
  return true;
}

bool AppendExpireAt(std::string_view key, int64_t expire, std::string* output) {
  std::array<char, std::numeric_limits<int64_t>::digits10 + 3> buffer{};
  const auto encoded =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), expire);
  if (encoded.ec != std::errc()) {
    return false;
  }
  const std::string_view encoded_expire(
      buffer.data(), static_cast<size_t>(encoded.ptr - buffer.data()));
  if (!ReserveCommands({"PEXPIREAT", key, encoded_expire}, 1, output)) {
    return false;
  }
  reply::AppendArrayHeader(3, output);
  reply::AppendBulkString("PEXPIREAT", output);
  reply::AppendBulkString(key, output);
  reply::AppendBulkString(encoded_expire, output);
  return true;
}

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
bool AppendSnapshotRecord(std::string_view key, const db::RedisObject& object,
                          std::optional<int64_t> expire, const Limits& limits,
                          Sink* sink) {
  bool encoded = false;
  switch (object.Type()) {
    case db::RedisObject::ObjectType::kString:
      encoded = EmitString(key, object.String(), limits, sink);
      break;
    case db::RedisObject::ObjectType::kSet: {
      const set::Set* set = object.Set();
      SnapshotBatch batch("SADD", key, 1, limits, sink);
      encoded = set->ForEachMember([&batch](std::string_view member) {
        return batch.Append({member});
      }) && batch.Finish();
      break;
    }
    case db::RedisObject::ObjectType::kList: {
      const list::List* list = object.List();
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
      const zset::ZSet* zset = object.ZSet();
      SnapshotBatch batch("ZADD", key, 2, limits, sink);
      encoded = zset->ForEachEntry([&batch](std::string_view member,
                                            double score) {
        const std::string score_text = utils::FloatToString(score);
        return batch.Append({score_text, member});
      }) && batch.Finish();
      break;
    }
    case db::RedisObject::ObjectType::kHash: {
      const hash::Hash* hash = object.Hash();
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

template <typename Sink>
bool BuildSnapshotRecords(db::RedisDb* const db, const Limits& limits,
                          Sink* sink) {
  return db != nullptr && sink != nullptr &&
         db->ForEachObject([db, &limits, sink](std::string_view key,
                                               const db::RedisObject& object) {
           return AppendSnapshotRecord(key, object, db->Expiration(key), limits,
                                       sink);
         });
}
// Commands of one MULTI/EXEC block read during replay, held until EXEC.
class ReplayTransaction {
 public:
  void Queue(const command::Command* command,
             const command::CommandArgs& args) {
    commands_.push_back({command, args.size()});
    for (const std::string_view arg : args) {
      args_.emplace_back(arg);
    }
  }

  // Runs and clears the queued commands through `execute`, which returns
  // false when a command fails; Apply then stops and returns false.
  template <typename Execute>
  bool Apply(const Execute& execute) {
    command::CommandArgs views;
    size_t next_arg = 0;
    bool applied = true;
    for (const auto& [command, arg_count] : commands_) {
      views.assign(
          args_.begin() + static_cast<std::ptrdiff_t>(next_arg),
          args_.begin() + static_cast<std::ptrdiff_t>(next_arg + arg_count));
      next_arg += arg_count;
      if (!execute(command, &views)) {
        applied = false;
        break;
      }
    }
    commands_.clear();
    args_.clear();
    return applied;
  }

 private:
  struct QueuedCommand {
    const command::Command* command;
    size_t arg_count;
  };
  std::vector<QueuedCommand> commands_;
  std::vector<std::string> args_;
};
}  // namespace

std::string_view ErrorName(AofError error) {
  switch (error) {
    case AofError::kNone:
      return "none";
    case AofError::kWrite:
      return "write";
    case AofError::kSync:
      return "sync";
    case AofError::kRename:
      return "rename";
    case AofError::kDirectorySync:
      return "directory-sync";
    case AofError::kSnapshot:
      return "snapshot";
  }
  return "unknown";
}

bool Aof::Append(std::string_view command,
                 const std::vector<std::string_view>& args,
                 db::RedisDb* const db) {
  if (timestamp_enabled_) {
    // Redis' genAofTimestampAnnotationIfNeeded: one annotation per second.
    const int64_t now = UnixSeconds();
    if (now > current_timestamp_) {
      current_timestamp_ = now;
      AppendTimestamp(now, &batch_);
    }
  }
  if (!database_selected_) {
    // Redis' feedAppendOnlyFile: a SELECT when the database differs from
    // the last one written, which no file has yet at startup or after a
    // rewrite starts.
    database_selected_ = true;
    batch_.append(kSelectRecord);
  }
  const size_t batch_size = batch_.size();
  if (!Encode(command, args, db, &batch_)) {
    // Drop any partially encoded record so the batch stays replayable.
    batch_.resize(batch_size);
    return false;
  }
  if (in_transaction_) {
    ++transaction_records_;
  }
  return true;
}

void Aof::BeginTransaction() {
  in_transaction_ = true;
  transaction_start_ = batch_.size();
  transaction_records_ = 0;
}

void Aof::EndTransaction() {
  if (!in_transaction_) {
    return;
  }
  in_transaction_ = false;
  if (transaction_records_ < 2) {
    return;
  }
  batch_.insert(transaction_start_, kMultiRecord);
  batch_.append(kExecRecord);
}

bool Aof::Flush(bool force) {
  // Nothing runs between an EXEC's commands, and BGREWRITEAOF inside EXEC
  // is scheduled, so a transaction's records are never split.
  RS_CHECK(!in_transaction_);
  LogSyncFailure();
  const auto now = std::chrono::steady_clock::now();
  bool sync_in_progress = false;
  if (batch_.empty()) {
    if (increment_fsync_offset_ == increment_size_) {
      return true;
    }
    // Written but not yet synced: with `everysec`, sync once a second even
    // when no new writes come; with `always`, after a policy change.
    if (fsync_ == FsyncPolicy::kEverySecond) {
      if (now - last_fsync_ < kSyncInterval || FsyncInProgress()) {
        return true;
      }
    } else if (fsync_ != FsyncPolicy::kAlways) {
      return true;
    }
  } else {
    if (fsync_ == FsyncPolicy::kEverySecond) {
      sync_in_progress = FsyncInProgress();
    }
    if (fsync_ == FsyncPolicy::kEverySecond && !force && sync_in_progress) {
      // On Linux a write blocks behind a running fsync of the same file, so
      // wait for it, but not for more than two seconds.
      if (!flush_postponed_start_.has_value()) {
        flush_postponed_start_ = now;
        return true;
      }
      if (now - *flush_postponed_start_ < kMaxFlushPostpone) {
        return true;
      }
      ++delayed_fsyncs_;
      RS_LOG_INFO(
          "Asynchronous AOF fsync is taking too long (disk is busy?). Writing "
          "the AOF buffer without waiting for fsync to complete, this may "
          "slow down the server.\n");
    }
    const ssize_t written = WriteAll(file_ops_.get(), fd_, batch_);
    flush_postponed_start_.reset();
    if (std::cmp_not_equal(written, batch_.size())) {
      FailWrite(written);
      // With `always` the replies already promise durability, so stop.
      return fsync_ != FsyncPolicy::kAlways;
    }
    if (write_failed_) {
      RS_LOG_INFO(
          "AOF write error looks solved, the server can write again.\n");
      write_failed_ = false;
    }
    current_size_ += batch_.size();
    increment_size_ += batch_.size();
    batch_.clear();
  }

  if (no_fsync_on_rewrite_ && rewrite_child_ > 0) {
    return true;
  }
  if (fsync_ == FsyncPolicy::kAlways) {
    if (!SyncFile(fd_)) {
      RS_LOG_ERROR(
          "Can't persist AOF for fsync error when the AOF fsync policy is "
          "'always': %s. Exiting...\n",
          std::strerror(errno));
      return false;
    }
    increment_fsync_offset_ = increment_size_;
    last_fsync_ = now;
  } else if (fsync_ == FsyncPolicy::kEverySecond &&
             now - last_fsync_ >= kSyncInterval) {
    if (!sync_in_progress) {
      QueueFsync(fd_, false);
      increment_fsync_offset_ = increment_size_;
    }
    last_fsync_ = now;
  }
  return true;
}

void Aof::FailWrite(ssize_t written) {
  const auto now = std::chrono::steady_clock::now();
  // Log at most once per 30 seconds, as Redis does.
  const bool can_log =
      now - last_write_error_log_ > kWriteErrorLogInterval ||
      last_write_error_log_ == std::chrono::steady_clock::time_point{};
  if (can_log) {
    last_write_error_log_ = now;
  }
  if (written < 0) {
    write_errno_ = errno;
    if (can_log) {
      RS_LOG_ERROR("Error writing to the AOF file: %s\n",
                   std::strerror(write_errno_));
    }
  } else {
    if (can_log) {
      RS_LOG_ERROR(
          "Short write while writing to the AOF file: (nwritten=%zd, "
          "expected=%zu)\n",
          written, batch_.size());
    }
    // Cut the partial write off, so the whole buffer is retried and a
    // crash meanwhile leaves no torn record.
    if (increment_size_ <=
            static_cast<uintmax_t>(std::numeric_limits<off_t>::max()) &&
        ftruncate(fd_, static_cast<off_t>(increment_size_)) == 0) {
      written = -1;
    } else if (can_log) {
      RS_LOG_ERROR(
          "Could not remove short write from the append-only file. The server "
          "may refuse to load the AOF the next time it starts. ftruncate: "
          "%s\n",
          std::strerror(errno));
    }
    write_errno_ = ENOSPC;
  }
  write_failed_ = true;
  if (written > 0) {
    // The written prefix stays in the file; keep only the rest.
    const auto size = static_cast<size_t>(written);
    current_size_ += size;
    increment_size_ += size;
    batch_.erase(0, size);
  }
}

bool Aof::AcceptingWrites() const {
  return !write_failed_ && !fsync_failed_.load(std::memory_order_relaxed) &&
         !manifest_unsafe_;
}

std::string Aof::LastErrorText() const {
  int error_number = write_errno_;
  if (!write_failed_ && fsync_failed_.load(std::memory_order_relaxed)) {
    error_number = fsync_errno_.load(std::memory_order_relaxed);
  } else if (!write_failed_ && manifest_unsafe_) {
    error_number = manifest_errno_;
  }
  return error_number != 0 ? std::strerror(error_number) : "unknown error";
}

bool Aof::SyncNow() {
  // Redis' shutdown: a forced flush, then an fsync whatever the policy.
  if (fd_ < 0) {
    return false;
  }
  const bool flushed = Flush(true) && batch_.empty();
  if (!SyncFile(fd_)) {
    return false;
  }
  increment_fsync_offset_ = increment_size_;
  return flushed;
}

bool Aof::FsyncInProgress() const {
  return pending_fsyncs_.load(std::memory_order_acquire) != 0;
}

void Aof::QueueFsync(int fd, bool close_after) {
  if (!close_after) {
    pending_fsyncs_.fetch_add(1, std::memory_order_acq_rel);
  }
  {
    const std::scoped_lock lock(mutex_);
    sync_jobs_.push_back({fd, close_after});
  }
  jobs_queued_.notify_all();
}

void Aof::QueueDeletion(const std::string& path) {
  DeletionPath copy{};
  if (path.size() >= copy.size()) {
    // Too long for the fixed buffer: delete it here instead.
    unlink(path.c_str());
    return;
  }
  std::ranges::copy(path, copy.begin());
  {
    const std::scoped_lock lock(mutex_);
    deletions_.push_back(copy);
  }
  jobs_queued_.notify_all();
}

void Aof::WaitForBackgroundJobs() {
  std::unique_lock<std::mutex> lock(mutex_);
  jobs_done_.wait(lock, [this] {
    return next_sync_job_ == sync_jobs_.size() && deletions_.empty() &&
           !running_job_;
  });
}

void Aof::RunJob(int fd, bool close_after) {
  // Redis' bio worker for BIO_AOF_FSYNC and BIO_CLOSE_AOF. A file already
  // closed is not a failed sync.
  if (!SyncFile(fd) && errno != EBADF && errno != EINVAL) {
    fsync_errno_.store(errno, std::memory_order_relaxed);
    fsync_failed_.store(true, std::memory_order_release);
  } else {
    fsync_failed_.store(false, std::memory_order_release);
  }
  if (close_after) {
    close(fd);
  } else {
    pending_fsyncs_.fetch_sub(1, std::memory_order_acq_rel);
  }
}

void Aof::RunBackgroundThread() {
  // Like Redis' bio threads: it syncs and closes AOF files, and deletes
  // files a rewrite retired. It never allocates or frees memory, so the
  // rewrite's fork can happen while it runs, and it does not log, since
  // formatting a message allocates.
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    jobs_queued_.wait(lock, [this] {
      return stopping_ || next_sync_job_ < sync_jobs_.size() ||
             !deletions_.empty();
    });
    running_job_ = true;
    while (next_sync_job_ < sync_jobs_.size()) {
      const SyncJob job = sync_jobs_[next_sync_job_++];
      lock.unlock();
      RunJob(job.fd, job.close_after);
      lock.lock();
    }
    // Keeps the capacity: clearing trivially destructible jobs frees nothing.
    sync_jobs_.clear();
    next_sync_job_ = 0;
    while (!deletions_.empty()) {
      const DeletionPath path = deletions_.back();
      deletions_.pop_back();
      lock.unlock();
      unlink(path.data());
      lock.lock();
    }
    running_job_ = false;
    jobs_done_.notify_all();
    if (stopping_) {
      return;
    }
  }
}

void Aof::LogSyncFailure() {
  const bool failed = fsync_failed_.load(std::memory_order_acquire);
  if (failed && !fsync_failure_logged_) {
    RS_LOG_ERROR("Fail to fsync the AOF file: %s\n",
                 std::strerror(fsync_errno_.load(std::memory_order_relaxed)));
  }
  fsync_failure_logged_ = failed;
}

void Aof::RecordError(AofError error, int error_number) {
  RS_LOG_ERROR("AOF %.*s failed: %s\n",
               static_cast<int>(ErrorName(error).size()),
               ErrorName(error).data(),
               error_number != 0 ? std::strerror(error_number) : "unknown");
}

int Aof::WriteSnapshotFile(db::RedisDb* const db,
                           const std::string& path) const {
  // Redis' rewriteAppendOnlyFile. In the rewrite child it takes no locks and
  // does not log, since another thread may have held one at the fork;
  // allocating is safe because the parent forks only while no other thread
  // allocates.
  int open_flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  const int fd = open(path.c_str(), open_flags, 0644);
  if (fd < 0) {
    return kChildWriteFailed;
  }
  int status = kChildSucceeded;
  size_t unsynced_bytes = 0;
  const auto write_block = [this, fd, &status,
                            &unsynced_bytes](std::string& block) {
    std::array<std::string, 1> blocks{std::move(block)};
    block.clear();
    if (!WriteBlocks(file_ops_.get(), fd, blocks)) {
      status = kChildWriteFailed;
      return false;
    }
    // aof-rewrite-incremental-fsync: sync as the file grows, so the final
    // sync does not stall on all of it at once.
    unsynced_bytes += blocks[0].size();
    if (rewrite_incremental_fsync_ && unsynced_bytes >= kAutoSyncBytes) {
      unsynced_bytes = 0;
      if (!SyncFile(fd)) {
        status = kChildSyncFailed;
        return false;
      }
    }
    return true;
  };
  std::string block;
  if (timestamp_enabled_) {
    AppendTimestamp(UnixSeconds(), &block);
  }
  if (db->KeyCount() > 0) {
    block.append(kSelectRecord);
  }
  auto sink = [this, &block, &write_block](std::string record) {
    if (block.empty()) {
      block = std::move(record);
    } else {
      block.append(record);
    }
    return block.size() < limits_.snapshot_block_bytes || write_block(block);
  };
  if (!BuildSnapshotRecords(db, limits_, &sink)) {
    if (status == kChildSucceeded) {
      status = kChildSnapshotFailed;
    }
  } else if (write_block(block) && !SyncFile(fd)) {
    status = kChildSyncFailed;
  }
  if (close(fd) < 0 && status == kChildSucceeded) {
    status = kChildWriteFailed;
  }
  if (status != kChildSucceeded) {
    unlink(path.c_str());
  }
  return status;
}

std::string Aof::RewriteTempPath(pid_t child) const {
  // Redis' temp-rewriteaof-bg-<pid>.aof, in the AOF directory rather than
  // Redis' working directory, since --appenddirname may name another file
  // system.
  return FilePath(std::string(kTempPrefix) + "rewriteaof-bg-" +
                  std::to_string(child) + ".aof");
}

void Aof::RunRewriteChild(db::RedisDb* const db) const {
  // The child writes temp-rewriteaof-<pid>.aof and renames it to
  // temp-rewriteaof-bg-<pid>.aof once it is complete and synced.
  const pid_t self = getpid();
  const std::string path = FilePath(std::string(kTempPrefix) + "rewriteaof-" +
                                    std::to_string(self) + ".aof");
  int status = WriteSnapshotFile(db, path);
  if (status == kChildSucceeded &&
      std::rename(path.c_str(), RewriteTempPath(self).c_str()) < 0) {
    unlink(path.c_str());
    status = kChildWriteFailed;
  }
  _exit(status);
}

void Aof::CheckRewrite() {
  if (rewrite_child_ <= 0) {
    return;
  }
  int wait_status = 0;
  const pid_t result = WaitForChild(rewrite_child_, &wait_status, WNOHANG);
  if (result != 0) {
    FinishRewrite(result < 0 ? -1 : wait_status);
  }
}

void Aof::WaitForRewrite() {
  if (rewrite_child_ <= 0) {
    return;
  }
  int wait_status = 0;
  const pid_t result = WaitForChild(rewrite_child_, &wait_status, 0);
  FinishRewrite(result < 0 ? -1 : wait_status);
}

bool Aof::SyncFile(int fd) const { return aof::SyncFile(file_ops_.get(), fd); }

bool Aof::SyncParentDirectory() const {
  // Renames of the manifest and base files happen inside the directory.
  int open_flags = O_RDONLY;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
  open_flags |= O_DIRECTORY;
#endif
  const int directory_fd = open(directory_.c_str(), open_flags);
  if (directory_fd < 0) {
    return false;
  }
  const bool synced = SyncFile(directory_fd);
  const bool closed = close(directory_fd) == 0;
  return synced && closed;
}

std::string Manifest::Encode() const {
  std::string text;
  const auto append = [&text](const ManifestFile& file) {
    text.append("file ");
    if (NeedsQuoting(file.name)) {
      AppendQuoted(file.name, &text);
    } else {
      text.append(file.name);
    }
    text.append(" seq ")
        .append(std::to_string(file.sequence))
        .append(" type ")
        .push_back(TypeCode(file.type));
    if (file.start_offset.has_value()) {
      text.append(" startoffset ").append(std::to_string(*file.start_offset));
      if (file.end_offset.has_value()) {
        text.append(" endoffset ").append(std::to_string(*file.end_offset));
      }
    }
    text.push_back('\n');
  };
  if (base.has_value()) {
    append(*base);
  }
  for (const ManifestFile& file : history) {
    append(file);
  }
  for (const ManifestFile& file : increments) {
    append(file);
  }
  return text;
}

std::optional<Manifest> Manifest::Parse(std::string_view text) {
  // Redis reads lines of at most this many bytes.
  constexpr size_t kMaxLineBytes = 1024;
  if (text.empty()) {
    return std::nullopt;
  }
  Manifest manifest;
  int64_t max_increment_sequence = 0;
  while (!text.empty()) {
    const size_t line_end = text.find('\n');
    const bool has_newline = line_end != std::string_view::npos;
    std::string_view line = text.substr(0, line_end);
    text = has_newline ? text.substr(line_end + 1) : std::string_view();
    if (!line.empty() && line.front() == '#') {
      continue;
    }
    if (!has_newline || line.size() + 1 > kMaxLineBytes) {
      return std::nullopt;
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' ||
                             line.front() == '\r')) {
      line.remove_prefix(1);
    }
    while (!line.empty() &&
           (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      return std::nullopt;
    }
    // Key/value pairs in any order; at least the three Redis requires, and
    // unknown keys are ignored for forward compatibility.
    const std::optional<std::vector<std::string>> fields = SplitFields(line);
    if (!fields.has_value() || fields->size() < 6 || fields->size() % 2 != 0) {
      return std::nullopt;
    }
    ManifestFile file;
    bool has_name = false;
    char type_code = '\0';
    for (size_t index = 0; index < fields->size(); index += 2) {
      const std::string& key = (*fields)[index];
      const std::string& value = (*fields)[index + 1];
      if (EqualsIgnoringCase(key, "file")) {
        if (value.find_first_of("/\\") != std::string::npos) {
          return std::nullopt;
        }
        file.name = value;
        has_name = true;
      } else if (EqualsIgnoringCase(key, "seq")) {
        file.sequence = ParseLeadingInteger(value);
      } else if (EqualsIgnoringCase(key, "type")) {
        type_code = value.empty() ? '\0' : value.front();
      } else if (EqualsIgnoringCase(key, "startoffset")) {
        file.start_offset = ParseLeadingInteger(value);
      } else if (EqualsIgnoringCase(key, "endoffset")) {
        file.end_offset = ParseLeadingInteger(value);
      }
    }
    if (!has_name || file.sequence == 0 || type_code == '\0') {
      return std::nullopt;
    }
    switch (type_code) {
      case 'b':
        if (manifest.base.has_value()) {
          return std::nullopt;
        }
        file.type = ManifestFile::Type::kBase;
        manifest.base_sequence = file.sequence;
        manifest.base = std::move(file);
        break;
      case 'h':
        file.type = ManifestFile::Type::kHistory;
        manifest.history.push_back(std::move(file));
        break;
      case 'i':
        // Incremental files replay in order.
        if (file.sequence <= max_increment_sequence) {
          return std::nullopt;
        }
        file.type = ManifestFile::Type::kIncrement;
        manifest.increment_sequence = file.sequence;
        max_increment_sequence = file.sequence;
        manifest.increments.push_back(std::move(file));
        break;
      default:
        return std::nullopt;
    }
  }
  return manifest;
}

std::unique_ptr<Aof> Aof::Open(const Options& options, db::RedisDb* const db) {
  if (db == nullptr || options.directory.empty() ||
      !ValidFileName(options.file_name) ||
      options.limits.max_replay_command_bytes == 0 ||
      options.limits.snapshot_block_bytes == 0) {
    return nullptr;
  }
  // Redis' startup: load the manifest (upgrading a single-file AOF), replay
  // the files, create a base file if there is none, open the last
  // incremental file, and delete history files.
  auto aof = std::unique_ptr<Aof>(new Aof(options, options.directory));
  if (!aof->LoadManifest() || !aof->Replay(db)) {
    return nullptr;
  }
  if (mkdir(options.directory.c_str(), 0755) < 0 && errno != EEXIST) {
    RS_LOG_ERROR("Can't open or create append-only dir %s: %s\n",
                 options.directory.c_str(), std::strerror(errno));
    return nullptr;
  }
  // The project does not throw, but std::thread reports a failure to start
  // only by throwing std::system_error; catch it at this boundary so the
  // caller sees an ordinary error.
  try {
    aof->background_thread_ = std::thread(
        [instance = aof.get()] { instance->RunBackgroundThread(); });
  } catch (const std::system_error&) {
    return nullptr;
  }
  if (!aof->CreateBaseIfMissing(db) || !aof->OpenLastIncrement()) {
    return nullptr;
  }
  aof->DeleteHistory();
  return aof;
}

Aof::Aof(const Options& options, std::string directory)
    : directory_(std::move(directory)),
      file_name_(options.file_name),
      fsync_(options.fsync),
      auto_rewrite_min_bytes_(options.auto_rewrite_min_bytes),
      auto_rewrite_percentage_(options.auto_rewrite_percentage),
      limits_(options.limits),
      load_truncated_(options.load_truncated),
      load_corrupt_tail_max_bytes_(options.load_corrupt_tail_max_bytes),
      timestamp_enabled_(options.timestamp_enabled),
      rewrite_incremental_fsync_(options.rewrite_incremental_fsync),
      no_fsync_on_rewrite_(options.no_fsync_on_rewrite),
      file_ops_(options.file_ops != nullptr ? options.file_ops
                                            : DefaultFileOps()),
      last_fsync_(std::chrono::steady_clock::now()) {}

Aof::~Aof() {
  // Redis' shutdown: kill a running rewrite, whether or not its child has
  // finished, then write and sync everything pending.
  if (rewrite_child_ > 0) {
    AbandonRewrite();
  }
  SyncNow();
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  jobs_queued_.notify_all();
  if (background_thread_.joinable()) {
    background_thread_.join();
  }
  // Jobs queued when the background thread never started.
  for (; next_sync_job_ < sync_jobs_.size(); ++next_sync_job_) {
    RunJob(sync_jobs_[next_sync_job_].fd,
           sync_jobs_[next_sync_job_].close_after);
  }
  for (const DeletionPath& path : deletions_) {
    unlink(path.data());
  }
  if (fd_ >= 0) {
    close(fd_);
  }
}

bool Aof::CutTail(int fd, size_t valid_bytes) const {
  // Redis' truncateAppendOnlyFile.
  if (valid_bytes > static_cast<uintmax_t>(std::numeric_limits<off_t>::max()) ||
      ftruncate(fd, static_cast<off_t>(valid_bytes)) < 0) {
    RS_LOG_ERROR("Error truncating the AOF file: %s\n", std::strerror(errno));
    return false;
  }
  return SyncFile(fd);
}

std::string Aof::FilePath(std::string_view name) const {
  std::string path = directory_;
  path.push_back('/');
  path.append(name);
  return path;
}

std::string Aof::ManifestPath() const {
  return FilePath(file_name_ + std::string(kManifestSuffix));
}

bool Aof::LoadManifest() {
  // Redis' aofLoadManifestFromDisk: no directory or no manifest is an empty
  // AOF.
  const bool directory_exists = IsDirectory(directory_);
  if (directory_exists && PathExists(ManifestPath())) {
    const std::optional<std::string> text = ReadWholeFile(ManifestPath());
    if (!text.has_value()) {
      RS_LOG_ERROR("Fatal error: can't open the AOF manifest file %s\n",
                   ManifestPath().c_str());
      return false;
    }
    std::optional<Manifest> manifest = Manifest::Parse(*text);
    if (!manifest.has_value()) {
      RS_LOG_ERROR("Invalid AOF manifest file format: %s\n",
                   ManifestPath().c_str());
      return false;
    }
    manifest_ = std::move(*manifest);
  }
  // Redis' upgradeAofIfNeeded: a single-file AOF in the working directory
  // becomes the base file, when there is no AOF directory, when its manifest
  // lists no files, or when an earlier upgrade listed it but did not move
  // it yet.
  if (!PathExists(file_name_)) {
    return true;
  }
  const bool upgrade =
      !directory_exists ||
      (!manifest_.base.has_value() && manifest_.increments.empty()) ||
      (manifest_.base.has_value() && manifest_.increments.empty() &&
       manifest_.base->name == file_name_ && !PathExists(FilePath(file_name_)));
  if (!upgrade) {
    return true;
  }
  // Redis' aofUpgradePrepare: list the file first, then move it, so a crash
  // in between repeats the upgrade on the next start.
  if (mkdir(directory_.c_str(), 0755) < 0 && errno != EEXIST) {
    RS_LOG_ERROR("Can't open or create append-only dir %s: %s\n",
                 directory_.c_str(), std::strerror(errno));
    return false;
  }
  Manifest manifest = manifest_;
  manifest.base = MakeFile(file_name_, 1, ManifestFile::Type::kBase);
  manifest.base_sequence = 1;
  if (PersistManifest(manifest) != AofError::kNone) {
    return false;
  }
  manifest_ = std::move(manifest);
  if (std::rename(file_name_.c_str(), FilePath(file_name_).c_str()) < 0) {
    RS_LOG_ERROR("Error trying to move the old AOF file %s into dir %s: %s\n",
                 file_name_.c_str(), directory_.c_str(), std::strerror(errno));
    return false;
  }
  RS_LOG_INFO(
      "Successfully migrated an old-style AOF file (%s) into the AOF "
      "directory (%s).\n",
      file_name_.c_str(), directory_.c_str());
  return true;
}

bool Aof::Replay(db::RedisDb* const db) {
  // Redis' loadAppendOnlyFiles.
  std::vector<const ManifestFile*> files;
  if (manifest_.base.has_value()) {
    files.push_back(&*manifest_.base);
  }
  for (const ManifestFile& file : manifest_.increments) {
    files.push_back(&file);
  }
  // A listed file that is missing is fatal.
  size_t total_size = 0;
  for (const ManifestFile* const file : files) {
    const std::optional<size_t> size = PathSize(FilePath(file->name));
    if (!size.has_value()) {
      RS_LOG_ERROR("The append log file %s doesn't exist: %s\n",
                   file->name.c_str(), std::strerror(errno));
      return false;
    }
    total_size += *size;
  }
  if (total_size == 0) {
    return true;
  }
  for (size_t index = 0; index < files.size(); ++index) {
    int open_flags = O_RDWR;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    const std::string path = FilePath(files[index]->name);
    const int fd = open(path.c_str(), open_flags);
    if (fd < 0) {
      RS_LOG_ERROR(
          "Fatal error: can't open the append log file %s for reading: %s\n",
          files[index]->name.c_str(), std::strerror(errno));
      return false;
    }
    if (files[index]->type == ManifestFile::Type::kBase) {
      base_size_ = FileSize(fd).value_or(0);
    }
    // Only the last file may end in a torn record.
    const bool replayed = ReplayFile(db, fd, index + 1 == files.size());
    close(fd);
    if (!replayed) {
      RS_LOG_ERROR("cannot load AOF file %s\n", path.c_str());
      return false;
    }
  }
  current_size_ = total_size;
  return true;
}

bool Aof::CreateBaseIfMissing(db::RedisDb* const db) {
  // Redis' aofOpenIfNeededOnServerStart: an AOF with no files starts with a
  // base file holding the current dataset.
  if (manifest_.base.has_value() || !manifest_.increments.empty()) {
    return true;
  }
  const int64_t sequence = manifest_.base_sequence + 1;
  const std::string name =
      file_name_ + "." + std::to_string(sequence) + std::string(kBaseSuffix);
  const std::string temp_path =
      FilePath(std::string(kTempPrefix) + "rewriteaof-" +
               std::to_string(getpid()) + ".aof");
  if (WriteSnapshotFile(db, temp_path) != kChildSucceeded) {
    RS_LOG_ERROR("Write error writing append only file on disk: %s\n",
                 std::strerror(errno));
    return false;
  }
  if (std::rename(temp_path.c_str(), FilePath(name).c_str()) < 0) {
    RS_LOG_ERROR(
        "Error moving temp append only file on the final destination: %s\n",
        std::strerror(errno));
    unlink(temp_path.c_str());
    return false;
  }
  manifest_.base = MakeFile(name, sequence, ManifestFile::Type::kBase);
  manifest_.base_sequence = sequence;
  RS_LOG_INFO("Creating AOF base file %s on server start\n", name.c_str());
  return true;
}

bool Aof::OpenLastIncrement() {
  // Appends continue in the last incremental file, as in Redis; only a
  // rewrite starts a new one.
  Manifest manifest = manifest_;
  const bool created = manifest.increments.empty();
  if (created) {
    const int64_t sequence = ++manifest.increment_sequence;
    manifest.increments.push_back(
        MakeFile(file_name_ + "." + std::to_string(sequence) +
                     std::string(kIncrementSuffix),
                 sequence, ManifestFile::Type::kIncrement));
  }
  const std::string& name = manifest.increments.back().name;
  int open_flags = O_WRONLY | O_APPEND | O_CREAT;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  const int fd = open(FilePath(name).c_str(), open_flags, 0644);
  if (fd < 0) {
    RS_LOG_ERROR("Can't open the append-only file %s: %s\n", name.c_str(),
                 std::strerror(errno));
    return false;
  }
  // A new incremental file, and a base CreateBaseIfMissing just made, which
  // only happens along with one, still need listing.
  if (created && PersistManifest(manifest) != AofError::kNone) {
    close(fd);
    return false;
  }
  manifest_ = std::move(manifest);
  fd_ = fd;
  increment_size_ = FileSize(fd).value_or(0);
  increment_fsync_offset_ = increment_size_;
  RS_LOG_INFO("%s AOF incr file %s on server start\n",
              created ? "Creating" : "Opening", name.c_str());
  return true;
}

bool Aof::OpenNextIncrement() {
  // Redis' openNewIncrAofForAppend.
  Manifest manifest = manifest_;
  const int64_t sequence = ++manifest.increment_sequence;
  const std::string name = file_name_ + "." + std::to_string(sequence) +
                           std::string(kIncrementSuffix);
  int open_flags = O_WRONLY | O_APPEND | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  const int fd = open(FilePath(name).c_str(), open_flags, 0644);
  if (fd < 0) {
    RS_LOG_ERROR("Can't open the append-only file %s: %s\n", name.c_str(),
                 std::strerror(errno));
    return false;
  }
  manifest.increments.push_back(
      MakeFile(name, sequence, ManifestFile::Type::kIncrement));
  const AofError error = PersistManifest(manifest);
  const int error_number = errno;
  if (error != AofError::kNone && error != AofError::kDirectorySync) {
    close(fd);
    return false;
  }
  RS_LOG_INFO("Creating AOF incr file %s on background rewrite\n",
              name.c_str());
  // After the rename the new manifest is live, durable or not.
  manifest_ = std::move(manifest);
  SwitchFile(fd);
  if (error == AofError::kDirectorySync) {
    FailDirectorySync(error_number);
  }
  return true;
}

void Aof::FailDirectorySync(int error_number) {
  // A rename that may not survive a crash. Redis treats it as an ordinary
  // failure and deletes the new files the live manifest lists; instead,
  // reject writes and keep the replaced files until a later manifest
  // persists safely.
  manifest_unsafe_ = true;
  manifest_errno_ = error_number;
  RecordError(AofError::kDirectorySync, error_number);
}

void Aof::SwitchFile(int fd) {
  // The background thread syncs and closes the replaced file, so the switch
  // does not wait for the disk.
  if (fd_ >= 0) {
    QueueFsync(fd_, true);
    last_fsync_ = std::chrono::steady_clock::now();
  }
  fd_ = fd;
  increment_size_ = 0;
  increment_fsync_offset_ = 0;
}

AofError Aof::PersistManifest(const Manifest& manifest) {
  const std::string temp_path = FilePath(std::string(kTempPrefix) + file_name_ +
                                         std::string(kManifestSuffix));
  int open_flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  const int fd = open(temp_path.c_str(), open_flags, 0644);
  if (fd < 0) {
    RS_LOG_ERROR("Can't open the temporary manifest file %s: %s\n",
                 temp_path.c_str(), std::strerror(errno));
    return AofError::kWrite;
  }
  const std::string text = manifest.Encode();
  int error_number = 0;
  const bool written =
      WritePrefix(file_ops_.get(), fd, text, &error_number) == text.size();
  const bool synced = written && SyncFile(fd);
  if (!synced && error_number == 0) {
    error_number = errno;
  }
  close(fd);
  if (!written || !synced) {
    RS_LOG_ERROR("Error writing the temporary manifest file %s: %s\n",
                 temp_path.c_str(), std::strerror(error_number));
    unlink(temp_path.c_str());
    errno = error_number;
    return written ? AofError::kSync : AofError::kWrite;
  }
  if (file_ops_->Rename(temp_path.c_str(), ManifestPath().c_str()) < 0) {
    const int rename_errno = errno;
    RS_LOG_ERROR(
        "Error trying to rename the temporary manifest file %s into %s: %s\n",
        temp_path.c_str(), ManifestPath().c_str(), std::strerror(rename_errno));
    unlink(temp_path.c_str());
    errno = rename_errno;
    return AofError::kRename;
  }
  if (!SyncParentDirectory()) {
    RS_LOG_ERROR("Fail to fsync manifest directory %s: %s.\n",
                 directory_.c_str(), std::strerror(errno));
    return AofError::kDirectorySync;
  }
  // Durable: a manifest a crash could have brought back is gone for good.
  manifest_unsafe_ = false;
  return AofError::kNone;
}

void Aof::DeleteHistory() {
  // Redis' aofDelHistoryFiles. After a manifest whose rename may not be
  // durable, a crash could bring back the previous manifest, which still
  // needs these files.
  if (manifest_.history.empty() || manifest_unsafe_) {
    return;
  }
  for (const ManifestFile& file : manifest_.history) {
    RS_LOG_INFO("Removing the history file %s in the background\n",
                file.name.c_str());
    QueueDeletion(FilePath(file.name));
  }
  manifest_.history.clear();
  // A failure does not matter: history files are never loaded.
  PersistManifest(manifest_);
}

void Aof::ScheduleRewrite() {
  rewrite_scheduled_ = true;
  // A manual rewrite is not held back by earlier failures.
  consecutive_rewrite_failures_ = 0;
}

RewriteResult Aof::StartRewrite(db::RedisDb* const db) {
  if (db == nullptr) {
    return RewriteResult::kError;
  }
  if (rewrite_child_ > 0) {
    return RewriteResult::kInProgress;
  }
  if (mkdir(directory_.c_str(), 0755) < 0 && errno != EEXIST) {
    RS_LOG_ERROR("Can't open or create append-only dir %s: %s\n",
                 directory_.c_str(), std::strerror(errno));
    last_rewrite_ok_ = false;
    return RewriteResult::kError;
  }
  // Every mutation applied to `db` must be in the current incremental file
  // before the switch: the snapshot covers them, and the next file must
  // not. Redis carries bytes a failed write left in the buffer into the new
  // file, where replay would apply them twice; wait them out instead.
  Flush(true);
  if (!batch_.empty()) {
    last_rewrite_ok_ = false;
    return RewriteResult::kError;
  }
  // Redis resets aof_selected_db before switching files, so the new file
  // starts with a SELECT.
  database_selected_ = false;
  if (!OpenNextIncrement()) {
    last_rewrite_ok_ = false;
    return RewriteResult::kError;
  }
  ++rewrites_;
  if (before_fork_) {
    before_fork_();
  }
  const pid_t child = fork();
  if (child == 0) {
    RunRewriteChild(db);
  }
  const int fork_errno = errno;
  if (after_fork_) {
    after_fork_();
  }
  if (child < 0) {
    last_rewrite_ok_ = false;
    RS_LOG_ERROR("Can't rewrite append only file in background: fork: %s\n",
                 std::strerror(fork_errno));
    return RewriteResult::kError;
  }
  RS_LOG_INFO("Background append only file rewriting started by pid %d\n",
              static_cast<int>(child));
  rewrite_scheduled_ = false;
  rewrite_child_ = child;
  rewrite_start_ = std::chrono::steady_clock::now();
  return RewriteResult::kStarted;
}

void Aof::FinishRewrite(int wait_status) {
  // Redis' backgroundRewriteDoneHandler.
  const pid_t child = rewrite_child_;
  rewrite_child_ = -1;
  if (wait_status != -1 && WIFEXITED(wait_status) &&
      WEXITSTATUS(wait_status) == kChildSucceeded) {
    RS_LOG_INFO("Background AOF rewrite terminated with success\n");
    int error_number = 0;
    const AofError error = InstallRewrite(child, &error_number);
    if (error == AofError::kNone) {
      last_rewrite_ok_ = true;
      consecutive_rewrite_failures_ = 0;
      RS_LOG_INFO("Background AOF rewrite finished successfully\n");
    } else {
      RecordError(error, error_number);
      last_rewrite_ok_ = false;
      ++consecutive_rewrite_failures_;
    }
  } else if (wait_status != -1 && WIFSIGNALED(wait_status)) {
    RS_LOG_WARN("Background AOF rewrite terminated by signal %d\n",
                WTERMSIG(wait_status));
    // SIGUSR1 is how a rewrite is stopped on purpose, not a failure.
    if (WTERMSIG(wait_status) != SIGUSR1) {
      last_rewrite_ok_ = false;
      ++consecutive_rewrite_failures_;
    }
  } else {
    RecordError(
        wait_status == -1 ? AofError::kSnapshot : ChildError(wait_status), 0);
    RS_LOG_WARN("Background AOF rewrite terminated with error\n");
    last_rewrite_ok_ = false;
    ++consecutive_rewrite_failures_;
  }
  RemoveRewriteTempFiles(child);
  last_rewrite_seconds_ = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - rewrite_start_)
                              .count();
}

AofError Aof::InstallRewrite(pid_t child, int* const error_number) {
  // The previous base and every incremental file but the current one become
  // history, in Redis' order: the incremental files first, then the base.
  Manifest manifest = manifest_;
  if (manifest.base.has_value()) {
    manifest.base->type = ManifestFile::Type::kHistory;
    manifest.history.insert(manifest.history.begin(),
                            std::move(*manifest.base));
  }
  const int64_t base_sequence = ++manifest.base_sequence;
  const std::string base_name = file_name_ + "." +
                                std::to_string(base_sequence) +
                                std::string(kBaseSuffix);
  manifest.base = MakeFile(base_name, base_sequence, ManifestFile::Type::kBase);
  if (file_ops_->Rename(RewriteTempPath(child).c_str(),
                        FilePath(base_name).c_str()) < 0) {
    *error_number = errno;
    RS_LOG_ERROR(
        "Error trying to rename the temporary AOF base file into %s: %s\n",
        base_name.c_str(), std::strerror(*error_number));
    return AofError::kRename;
  }
  if (manifest.increments.size() > 1) {
    std::vector<ManifestFile> rewritten(manifest.increments.begin(),
                                        manifest.increments.end() - 1);
    for (ManifestFile& file : rewritten) {
      file.type = ManifestFile::Type::kHistory;
    }
    manifest.history.insert(manifest.history.begin(),
                            std::make_move_iterator(rewritten.begin()),
                            std::make_move_iterator(rewritten.end()));
    manifest.increments.erase(manifest.increments.begin(),
                              manifest.increments.end() - 1);
  }
  const AofError persist_error = PersistManifest(manifest);
  if (persist_error != AofError::kNone &&
      persist_error != AofError::kDirectorySync) {
    *error_number = errno;
    // The old manifest still describes the AOF; drop the unused base.
    QueueDeletion(FilePath(base_name));
    return persist_error;
  }
  // After the rename the new manifest is live, durable or not.
  manifest_ = std::move(manifest);
  current_size_ = PathSize(FilePath(base_name)).value_or(0) + increment_size_;
  base_size_ = current_size_;
  if (persist_error == AofError::kDirectorySync) {
    *error_number = errno;
    FailDirectorySync(*error_number);
    return persist_error;
  }
  DeleteHistory();
  return AofError::kNone;
}

void Aof::RemoveRewriteTempFiles(pid_t child) {
  // Redis' aofRemoveTempFile.
  QueueDeletion(RewriteTempPath(child));
  QueueDeletion(FilePath(std::string(kTempPrefix) + "rewriteaof-" +
                         std::to_string(child) + ".aof"));
}

void Aof::AbandonRewrite() {
  // Redis' killAppendOnlyChild: stop the child, finished or not, and leave
  // the rewrite statistics as they were.
  RS_LOG_INFO("Killing running AOF rewrite child: %d\n",
              static_cast<int>(rewrite_child_));
  if (kill(rewrite_child_, SIGUSR1) == 0) {
    int wait_status = 0;
    WaitForChild(rewrite_child_, &wait_status, 0);
  }
  RemoveRewriteTempFiles(rewrite_child_);
  rewrite_child_ = -1;
}

bool Aof::RewriteLimited() {
  // Redis' aofRewriteLimited: after three consecutive failures, automatic
  // and scheduled rewrites wait a minute, then two, four, and so on up to an
  // hour. BGREWRITEAOF still starts one at once.
  if (consecutive_rewrite_failures_ < kFailuresBeforeBackoff) {
    next_delay_minutes_ = 0;
    next_rewrite_time_.reset();
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  if (next_rewrite_time_.has_value()) {
    if (now < *next_rewrite_time_) {
      return true;
    }
    next_rewrite_time_.reset();
    return false;
  }
  next_delay_minutes_ =
      next_delay_minutes_ == 0
          ? 1
          : std::min(next_delay_minutes_ * 2, kMaxBackoffMinutes);
  next_rewrite_time_ = now + std::chrono::minutes(next_delay_minutes_);
  RS_LOG_WARN(
      "Background AOF rewrite has repeatedly failed and triggered the limit, "
      "will retry in %" PRId64 " minutes\n",
      next_delay_minutes_);
  return true;
}

bool Aof::ShouldAutoRewrite() {
  // Redis' serverCron condition, with its integer growth percentage.
  if (rewrite_child_ > 0 || auto_rewrite_percentage_ == 0 ||
      current_size_ <= auto_rewrite_min_bytes_) {
    return false;
  }
  const uint64_t base = base_size_ != 0 ? base_size_ : 1;
  const auto current = static_cast<uint64_t>(current_size_);
  // current * 100 / base - 100, without overflow.
  const uint64_t ratio =
      ((current / base) * 100) + (((current % base) * 100) / base);
  if (ratio < 100 || ratio - 100 < auto_rewrite_percentage_) {
    return false;
  }
  return !RewriteLimited();
}

bool Aof::Cron(db::RedisDb* const db) {
  // A write postponed behind a sync, or a failed one, is retried here too.
  if (!Flush()) {
    return false;
  }
  if (rewrite_child_ <= 0 && rewrite_scheduled_ && !RewriteLimited()) {
    StartRewrite(db);
  }
  CheckRewrite();
  if (ShouldAutoRewrite()) {
    RS_LOG_INFO("Starting automatic rewriting of AOF on growth\n");
    StartRewrite(db);
  }
  return true;
}

AofState Aof::State() const {
  const auto now = std::chrono::steady_clock::now();
  return {
      .accepting_writes = AcceptingWrites(),
      .rewrite_in_progress = rewrite_child_ > 0,
      .rewrite_scheduled = rewrite_scheduled_,
      .last_rewrite_ok = last_rewrite_ok_,
      .last_rewrite_seconds = last_rewrite_seconds_,
      .current_rewrite_seconds =
          rewrite_child_ > 0 ? std::chrono::duration_cast<std::chrono::seconds>(
                                   now - rewrite_start_)
                                   .count()
                             : -1,
      .rewrites = rewrites_,
      .consecutive_rewrite_failures = consecutive_rewrite_failures_,
      .current_size = current_size_,
      .base_size = base_size_,
      .buffer_length = batch_.size(),
      .pending_fsyncs = pending_fsyncs_.load(std::memory_order_relaxed),
      .delayed_fsyncs = delayed_fsyncs_,
  };
}

bool Aof::ReplayFile(db::RedisDb* const db, int fd, bool last) {
  db->SetLoading(true);
  // Where a format error starts the corrupt tail: after the last complete
  // command, and before a transaction it interrupts.
  std::optional<size_t> corrupt_from;
  bool replayed = [this, db, fd, last, &corrupt_from] {
    Client client(db);
    std::array<char, kReadChunkSize> chunk{};
    in_memory::DynamicBuffer input;
    command::CommandArgs args;
    size_t complete_bytes = 0;
    bool reached_end = false;
    // Records between MULTI and EXEC are copied out of `input` and applied
    // together at EXEC, so a transaction cut off by a crash is not applied.
    std::optional<size_t> transaction_start;
    ReplayTransaction transaction;

    while (!reached_end) {
      ssize_t bytes_read = -1;
      while (true) {
        bytes_read = read(fd, chunk.data(), chunk.size());
        if (bytes_read >= 0 || errno != EINTR) {
          break;
        }
      }
      if (bytes_read < 0) {
        return false;
      }
      reached_end = bytes_read == 0;
      if (bytes_read > 0) {
        input.Append(chunk.data(), static_cast<size_t>(bytes_read));
      }

      while (!input.Empty()) {
        const std::string_view remaining = input.View();
        if (remaining.front() == '#') {
          // An annotation, such as "#TS:<unix time>"; Redis skips it.
          const size_t line_end = remaining.find('\n');
          if (line_end == std::string_view::npos && !reached_end) {
            if (remaining.size() > kMaxAnnotationBytes) {
              corrupt_from = transaction_start.value_or(complete_bytes);
              return false;
            }
            break;
          }
          const size_t line_size = line_end == std::string_view::npos
                                       ? remaining.size()
                                       : line_end + 1;
          if (!AddSize(line_size, &complete_bytes)) {
            return false;
          }
          input.Consume(line_size);
          continue;
        }
        if (remaining.front() != '*') {
          corrupt_from = transaction_start.value_or(complete_bytes);
          return false;
        }
        std::string_view name;
        const request_parser::ParseResult parsed =
            request_parser::Parse(remaining, &name, &args);
        if (parsed.status == request_parser::ParseStatus::kIncomplete) {
          break;
        }
        if (parsed.status != request_parser::ParseStatus::kComplete) {
          corrupt_from = transaction_start.value_or(complete_bytes);
          return false;
        }
        if (parsed.consumed > limits_.max_replay_command_bytes) {
          return false;
        }
        if (name == "SELECT" && args.size() == 1) {
          // The only database there is; any other would lose data.
          if (args[0] != "0") {
            RS_LOG_ERROR("AOF selects database %.*s, but only 0 exists\n",
                         static_cast<int>(args[0].size()), args[0].data());
            return false;
          }
        } else if (name == "MULTI" && args.empty()) {
          if (transaction_start.has_value()) {
            return false;
          }
          transaction_start = complete_bytes;
        } else if (name == "EXEC" && args.empty()) {
          const auto execute = [&client](const command::Command* command,
                                         command::CommandArgs* views) {
            return client.ExecuteForReplay(command, views);
          };
          if (!transaction_start.has_value() || !transaction.Apply(execute)) {
            return false;
          }
          transaction_start.reset();
        } else {
          const command::Command* metadata = command::Find(name);
          if (metadata == nullptr ||
              metadata->access != command::CommandAccess::kWrite ||
              !metadata->arity.Accepts(args.size())) {
            return false;
          }
          if (transaction_start.has_value()) {
            transaction.Queue(metadata, args);
          } else if (!client.ExecuteForReplay(metadata, &args)) {
            return false;
          }
        }
        if (!AddSize(parsed.consumed, &complete_bytes)) {
          return false;
        }
        input.Consume(parsed.consumed);
      }
      if (input.View().size() > limits_.max_replay_command_bytes) {
        return false;
      }
    }

    // Redis' unexpected end of file: a torn record, or a transaction without
    // its EXEC.
    const bool truncated = !input.Empty() || transaction_start.has_value();
    if (!truncated) {
      return true;
    }
    const size_t valid_bytes = transaction_start.value_or(complete_bytes);
    if (!load_truncated_) {
      RS_LOG_ERROR(
          "Unexpected end of file reading the append only file. You can set "
          "the 'aof-load-truncated' configuration option to yes and restart "
          "the server.\n");
      return false;
    }
    if (!last) {
      RS_LOG_ERROR("Fatal error: the truncated file is not the last file\n");
      return false;
    }
    RS_LOG_WARN("!!! Truncating the AOF at offset %zu !!!\n", valid_bytes);
    return CutTail(fd, valid_bytes);
  }();
  if (!replayed && corrupt_from.has_value()) {
    // Redis' aof-load-corrupt-tail-max-size: a format error with fewer than
    // that many bytes after the last complete command, in the last file.
    const size_t size = FileSize(fd).value_or(0);
    const size_t remaining = size - std::min(size, *corrupt_from);
    if (load_corrupt_tail_max_bytes_ > 0 &&
        remaining < load_corrupt_tail_max_bytes_) {
      if (!last) {
        RS_LOG_ERROR("Fatal error: the truncated file is not the last file\n");
      } else {
        RS_LOG_WARN(
            "!!! Warning: corrupt AOF file tail!!! Truncating the AOF at "
            "offset %zu (remaining %zu) !!!\n",
            *corrupt_from, remaining);
        replayed = CutTail(fd, *corrupt_from);
      }
    } else {
      RS_LOG_ERROR(
          "Bad file format reading the append only file at offset %zu. You "
          "can set the 'aof-load-corrupt-tail-max-size' configuration option "
          "to %zu and restart the server.\n",
          *corrupt_from, remaining);
    }
  }
  db->SetLoading(false);
  return replayed;
}

bool Aof::Encode(std::string_view command,
                 const std::vector<std::string_view>& args,
                 db::RedisDb* const db, std::string* const output) {
  if (db == nullptr || output == nullptr) {
    return false;
  }
  if (command == "SET") {
    if (args.empty()) {
      return false;
    }
    const auto [object, expire] = db->LookupKeyWithExpiration(args[0]);
    if (object == nullptr) {
      return AppendSingleKeyCommand("DEL", args[0], output);
    }
    if (object->Type() != db::RedisObject::ObjectType::kString) {
      return false;
    }
    if (!ReserveSetRecord(args[0], object->String(), expire.has_value(),
                          output)) {
      return false;
    }
    reply::AppendArrayHeader(3, output);
    reply::AppendBulkString("SET", output);
    reply::AppendBulkString(args[0], output);
    reply::AppendBulkString(object->String(), output);
    return !expire.has_value() || AppendExpireAt(args[0], *expire, output);
  }
  if (command == "EXPIRE" || command == "EXPIREAT" || command == "PEXPIRE" ||
      command == "PEXPIREAT") {
    if (args.empty()) {
      return false;
    }
    const std::optional<int64_t> expire = db->Expiration(args[0]);
    if (expire.has_value()) {
      return AppendExpireAt(args[0], *expire, output);
    }
    return AppendSingleKeyCommand("DEL", args[0], output);
  }
  return AppendCommand(command, args, output);
}

}  // namespace redis_simple::aof
