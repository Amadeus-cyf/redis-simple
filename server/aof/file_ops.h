#ifndef REDIS_SIMPLE_SERVER_AOF_FILE_OPS_H_
#define REDIS_SIMPLE_SERVER_AOF_FILE_OPS_H_

#include <sys/types.h>
#include <sys/uio.h>

#include <cerrno>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::aof {
// The file system calls the AOF makes where tests inject failures.
class FileOps {
 public:
  virtual ssize_t Write(int fd, const void* data, size_t size) = 0;
  virtual ssize_t WriteVector(int fd, const iovec* blocks, int count) = 0;
  virtual int Sync(int fd) = 0;
  virtual int Rename(const char* source, const char* destination) = 0;

  // The calls below tune how a snapshot reaches the disk. They are
  // not pure, and default to "not available", so file operations that only
  // inject failures keep the plain Sync path on every platform.

  // Whether StartWriteback and WaitForWriteback work (Linux'
  // sync_file_range).
  [[nodiscard]] virtual bool HasRangeSync() const { return false; }
  // Asks the kernel to start writing the range's dirty pages to the disk,
  // without waiting for them.
  virtual int StartWriteback(int fd, off_t offset, off_t size);
  // Blocks until the range's pages are on their way to the disk and that
  // write has finished. Unlike Sync it flushes no file metadata.
  virtual int WaitForWriteback(int fd, off_t offset, off_t size);
  // Asks the kernel to drop the file's pages that are already on disk from
  // its page cache (posix_fadvise's DONTNEED). Does nothing where the
  // platform has no such call.
  virtual int DropPageCache(int fd);

  virtual ~FileOps() = default;
};

// Keeps a snapshot that is written from the start of an empty file from
// piling up dirty pages, as the automatic sync of Redis' rioFileWrite does:
// after every kIntervalBytes written it pushes them towards the disk, so the
// final Sync has little left to do and never stalls other disk users for the
// whole file.
//
// With range sync (Linux) an interval only starts writing its own bytes and
// waits for the bytes of the interval before it, which have had a whole
// interval to reach the disk, so the writer rarely blocks. Elsewhere each
// interval is a full Sync. Either way it then drops the file's clean pages
// from the page cache, unless told to keep them, so a snapshot does not push
// the dataset's own memory out.
class IncrementalSync {
 public:
  // Redis' REDIS_AUTOSYNC_BYTES.
  static constexpr size_t kIntervalBytes = size_t{4} * 1024 * 1024;

  IncrementalSync(FileOps* file_ops, int fd, bool keep_page_cache)
      : file_ops_(file_ops), fd_(fd), keep_page_cache_(keep_page_cache) {}

  // Call after each `size` bytes written to the file. False, with errno
  // set, if a sync failed.
  bool Written(size_t size);

 private:
  bool SyncPending();

  FileOps* file_ops_;
  int fd_;
  bool keep_page_cache_;
  // Where the interval before the pending one starts, where the pending one
  // starts, and how many bytes it holds so far.
  off_t previous_offset_{};
  off_t pending_offset_{};
  size_t pending_bytes_{};
};

// The real system calls.
std::shared_ptr<FileOps> DefaultFileOps();

// open() with O_CLOEXEC, so the files never leak into a forked child that
// execs. Returns the descriptor, or -1 with errno set.
int OpenFile(const std::string& path, int flags, mode_t mode = 0);
// fsync, retried when interrupted.
bool SyncFile(FileOps* file_ops, int fd);
// Opens the directory and syncs it, so renames inside it survive a crash.
bool SyncDirectory(FileOps* file_ops, const std::string& directory);
// The size of an open file, or nullopt if it cannot be read.
std::optional<size_t> FileSize(int fd);
// The size of the file at `path`; nullopt, with errno set, if it is missing.
std::optional<size_t> PathSize(const std::string& path);
bool PathExists(const std::string& path);
bool IsDirectory(const std::string& path);
std::optional<std::string> ReadWholeFile(const std::string& path);

// Writes all of `data`, retrying short and interrupted writes, as Redis'
// aofWrite. Returns the bytes written, or -1 if an error stopped it before the
// first byte. After anything less than all of `data`, errno says why (ENOSPC
// when the file took no more without an error).
ssize_t WriteAll(FileOps* file_ops, int fd, std::string_view data);

// The most iovecs one writev() accepts.
size_t MaxIovCount();

// Writes every block (anything with data() and size()) with vectored writes,
// retrying short and interrupted writes.
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
}  // namespace redis_simple::aof

#endif  // REDIS_SIMPLE_SERVER_AOF_FILE_OPS_H_
