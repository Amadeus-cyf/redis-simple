#include "server/aof/file_ops.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace redis_simple::aof {
int FileOps::Close(int fd) { return close(fd); }

int FileOps::Unlink(const char* path) { return unlink(path); }

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

#ifdef __linux__
  [[nodiscard]] bool HasRangeSync() const override { return true; }

  int StartWriteback(int fd, off_t offset, off_t size) override {
    return sync_file_range(fd, offset, size, SYNC_FILE_RANGE_WRITE);
  }

  int WaitForWriteback(int fd, off_t offset, off_t size) override {
    return sync_file_range(fd, offset, size,
                           SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                               SYNC_FILE_RANGE_WAIT_AFTER);
  }

  int DropPageCache(int fd) override {
    // Offset and length 0 mean the whole file. posix_fadvise returns the
    // error number instead of setting errno.
    const int error_number = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    if (error_number != 0) {
      errno = error_number;
      return -1;
    }
    return 0;
  }
#endif
};
}  // namespace

int FileOps::StartWriteback(int /*fd*/, off_t /*offset*/, off_t /*size*/) {
  errno = ENOTSUP;
  return -1;
}

int FileOps::WaitForWriteback(int /*fd*/, off_t /*offset*/, off_t /*size*/) {
  errno = ENOTSUP;
  return -1;
}

int FileOps::DropPageCache(int /*fd*/) { return 0; }

bool IncrementalSync::Written(size_t size) {
  pending_bytes_ += size;
  if (pending_bytes_ < kIntervalBytes) {
    return true;
  }
  if (!SyncPending()) {
    return false;
  }
  if (!keep_page_cache_) {
    // The interval just started may still be on its way to the disk, and pages
    // being written are not dropped, so this asks for the whole file: each
    // interval's pages get another chance at the next one. A failure only
    // leaves pages cached.
    static_cast<void>(file_ops_->DropPageCache(fd_));
  }
  return true;
}

bool IncrementalSync::SyncPending() {
  const auto pending_size = static_cast<off_t>(pending_bytes_);
  if (!file_ops_->HasRangeSync()) {
    if (!SyncFile(file_ops_, fd_)) {
      return false;
    }
  } else {
    if (file_ops_->StartWriteback(fd_, pending_offset_, pending_size) < 0) {
      return false;
    }
    // Waiting for the interval before keeps the promise of incremental
    // sync: at most two intervals of dirty pages are ever waiting for
    // writeback.
    const off_t previous_size = pending_offset_ - previous_offset_;
    if (previous_size > 0 &&
        file_ops_->WaitForWriteback(fd_, previous_offset_, previous_size) < 0) {
      return false;
    }
  }
  previous_offset_ = pending_offset_;
  pending_offset_ += pending_size;
  pending_bytes_ = 0;
  return true;
}

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

void AdviseSequentialReads(int fd) {
#ifdef __linux__
  // A hint: loading goes on without it.
  posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
  static_cast<void>(fd);
#endif
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
  struct stat file_info = {};
  if (fstat(fd, &file_info) < 0 || file_info.st_size < 0 ||
      static_cast<uintmax_t>(file_info.st_size) >
          std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<size_t>(file_info.st_size);
}

std::optional<size_t> PathSize(const std::string& path) {
  struct stat info = {};
  if (stat(path.c_str(), &info) < 0) {
    return std::nullopt;
  }
  return info.st_size > 0 ? static_cast<size_t>(info.st_size) : 0;
}

bool PathExists(const std::string& path) {
  struct stat info = {};
  return stat(path.c_str(), &info) == 0;
}

bool IsDirectory(const std::string& path) {
  struct stat info = {};
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
}  // namespace redis_simple::aof
