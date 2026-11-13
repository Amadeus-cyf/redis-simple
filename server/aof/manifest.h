#ifndef REDIS_SIMPLE_SERVER_AOF_MANIFEST_H_
#define REDIS_SIMPLE_SERVER_AOF_MANIFEST_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::aof {
// One file of a multi-part AOF, as listed in its manifest.
struct ManifestFile {
  enum class Type {
    // The snapshot the last rewrite wrote.
    kBase,
    // Mutations appended since the base was written.
    kIncrement,
    // Replaced by a rewrite and deleted soon after.
    kHistory,
  };
  std::string name;
  int64_t sequence{};
  Type type{};
  // Redis' replication offsets for an incremental file. This server has no
  // replication and writes none, but keeps those of a Redis manifest.
  std::optional<int64_t> start_offset;
  std::optional<int64_t> end_offset;
};

// The manifest of a multi-part AOF, in Redis' format: one line per file, such
// as "file appendonly.aof.1.base.aof seq 1 type b". Loading replays the base
// file, then the incremental files in order.
struct Manifest {
  std::optional<ManifestFile> base;
  std::vector<ManifestFile> increments;
  std::vector<ManifestFile> history;
  // The sequence numbers of the base and the last incremental file, which the
  // next ones follow (Redis' curr_base_file_seq and curr_incr_file_seq).
  int64_t base_sequence{};
  int64_t increment_sequence{};

  // The base first, then history files, then incremental files, as Redis
  // writes them. Names with spaces, quotes, or unprintable bytes are quoted
  // and escaped, as Redis' sdscatrepr does.
  std::string Encode() const;
  // Parses a manifest as Redis' aofLoadManifestFromFile does, and returns
  // nullopt where Redis would refuse to start.
  static std::optional<Manifest> Parse(std::string_view text);
};

ManifestFile MakeManifestFile(std::string name, int64_t sequence,
                              ManifestFile::Type type);

// File names inside the AOF directory, as Redis 7 and later name them, for
// the configured file name `prefix` (appendfilename):
// "<prefix>.<sequence>.base.rdb" for an RDB base file (`rdb_format`) or
// "<prefix>.<sequence>.base.aof", "<prefix>.<sequence>.incr.aof", and
// "<prefix>.manifest". Temporary files start with "temp-".
std::string BaseFileName(std::string_view prefix, int64_t sequence,
                         bool rdb_format);
std::string IncrementFileName(std::string_view prefix, int64_t sequence);
std::string ManifestFileName(std::string_view prefix);
inline constexpr std::string_view kTempPrefix = "temp-";
// A name without a directory part, as Redis' pathIsBaseName requires of
// appendfilename.
bool ValidFileName(std::string_view name);
}  // namespace redis_simple::aof

#endif  // REDIS_SIMPLE_SERVER_AOF_MANIFEST_H_
