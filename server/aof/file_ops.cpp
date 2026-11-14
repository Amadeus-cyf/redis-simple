#include "server/aof/file_ops.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace redis_simple::aof {
namespace {
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
}  // namespace

std::shared_ptr<FileOps> DefaultFileOps() {
  // SystemFileOps is stateless, so each Aof can own its own instance.
  return std::make_shared<SystemFileOps>();
}

int OpenFile(const std::string& path, int flags, mode_t mode) {
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  return open(path.c_str(), flags, mode);
}

bool SyncFile(FileOps* const file_ops, int fd) {
  while (file_ops->Sync(fd) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

bool SyncDirectory(FileOps* const file_ops, const std::string& directory) {
  int flags = O_RDONLY;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
  const int directory_fd = OpenFile(directory, flags);
  if (directory_fd < 0) {
    return false;
  }
  const bool synced = SyncFile(file_ops, directory_fd);
  const bool closed = close(directory_fd) == 0;
  return synced && closed;
}

std::optional<size_t> FileSize(int fd) {
  struct stat file_info{};
  if (fstat(fd, &file_info) < 0 || file_info.st_size < 0 ||
      static_cast<uintmax_t>(file_info.st_size) >
          std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<size_t>(file_info.st_size);
}

std::optional<size_t> PathSize(const std::string& path) {
  struct stat info{};
  if (stat(path.c_str(), &info) < 0) {
    return std::nullopt;
  }
  return info.st_size > 0 ? static_cast<size_t>(info.st_size) : 0;
}

bool PathExists(const std::string& path) {
  struct stat info{};
  return stat(path.c_str(), &info) == 0;
}

bool IsDirectory(const std::string& path) {
  struct stat info{};
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::optional<std::string> ReadWholeFile(const std::string& path) {
  const int fd = OpenFile(path, O_RDONLY);
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
}  // namespace redis_simple::aof
