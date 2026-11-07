#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "server/aof/aof.h"
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
  // The bytes as a manifest.
  aof::Manifest::Parse(
      std::string_view(reinterpret_cast<const char*>(data), size));

  // The bytes after a valid record, as the only incremental file of a
  // multi-part AOF.
  constexpr std::string_view kTemplate = "/tmp/redis_aof_XXXXXX";
  std::array<char, kTemplate.size() + 1> root{};
  std::ranges::copy(kTemplate, root.begin());
  if (mkdtemp(root.data()) == nullptr) {
    return;
  }
  const std::string directory = std::string(root.data()) + "/appendonlydir";
  bool written = mkdir(directory.c_str(), 0755) == 0;
  if (written) {
    constexpr std::string_view kManifest =
        "file appendonly.aof.1.incr.aof seq 1 type i\n";
    const int manifest_fd =
        open((directory + "/appendonly.aof.manifest").c_str(),
             O_WRONLY | O_CREAT | O_TRUNC, 0644);
    written = manifest_fd >= 0 &&
              WriteAll(manifest_fd,
                       reinterpret_cast<const uint8_t*>(kManifest.data()),
                       kManifest.size());
    if (manifest_fd >= 0) {
      close(manifest_fd);
    }
  }
  if (written) {
    const int fd = open((directory + "/appendonly.aof.1.incr.aof").c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC, 0644);
    written =
        fd >= 0 &&
        WriteAll(fd, reinterpret_cast<const uint8_t*>(kValidCommand.data()),
                 kValidCommand.size()) &&
        WriteAll(fd, data, size);
    if (fd >= 0) {
      close(fd);
    }
  }
  if (written) {
    auto database = db::RedisDb::Create();
    auto append_only_file =
        aof::Aof::Open({directory, "appendonly.aof", aof::FsyncPolicy::kAlways},
                       database.get());
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
  std::error_code error;
  std::filesystem::remove_all(root.data(), error);
}
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzAof(data, size);
  return 0;
}
