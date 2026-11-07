#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "server/aof.h"
#include "server/db/db.h"

namespace redis_simple::fuzz {
namespace {
constexpr std::string_view kValidCommand =
    "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";

bool WriteAll(int fd, const uint8_t* data, size_t size) {
  size_t written = 0;
  while (written < size) {
    const ssize_t result = write(fd, data + written, size - written);
    if (result > 0) {
      written += static_cast<size_t>(result);
    } else if (result < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}
}  // namespace

void FuzzAof(const uint8_t* data, size_t size) {
  std::array<char, 32> path{};
  constexpr char kTemplate[] = "/tmp/redis_aof_XXXXXX";
  static_assert(sizeof(kTemplate) <= path.size());
  for (size_t index = 0; index < sizeof(kTemplate); ++index) {
    path[index] = kTemplate[index];
  }

  const int fd = mkstemp(path.data());
  if (fd < 0) {
    return;
  }
  const bool prefixed =
      WriteAll(fd, reinterpret_cast<const uint8_t*>(kValidCommand.data()),
               kValidCommand.size());
  const bool written = prefixed && WriteAll(fd, data, size);
  close(fd);
  if (written) {
    auto database = db::RedisDb::Create();
    auto append_only_file = aof::Aof::Open(
        {path.data(), aof::FsyncPolicy::kAlways}, database.get());
    if (append_only_file != nullptr) {
      std::string value;
      if (size > 0) {
        value.assign(reinterpret_cast<const char*>(data), size);
      }
      if (database->SetKey("fuzz", db::RedisObject::CreateWithString(value),
                           0) == db::DbStatus::kOk) {
        const std::vector<std::string_view> args = {"fuzz", value};
        append_only_file->Append("SET", args, database.get());
        if (append_only_file->AcceptingWrites()) {
          append_only_file->StartRewrite(database.get());
          append_only_file->WaitForRewrite();
        }
      }
    }
  }
  unlink(path.data());
}
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzAof(data, size);
  return 0;
}
