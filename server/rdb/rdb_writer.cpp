#include "server/rdb/rdb_writer.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "server/aof/file_ops.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/rdb/rdb_format.h"
#include "server/version.h"
#include "utils/crc64.h"
#include "utils/lzf.h"
#include "utils/strings.h"

namespace redis_simple::rdb {
namespace {
// The buffer is written once it holds this much.
constexpr size_t kBlockBytes = size_t{1024} * 1024;
// Payloads at least this large go to the file directly, not through the
// buffer, so a large value is not copied.
constexpr size_t kDirectWriteBytes = kBlockBytes;
// Redis' REDIS_AUTOSYNC_BYTES, for incremental fsync.
constexpr size_t kAutoSyncBytes = size_t{4} * 1024 * 1024;
// Redis compresses only strings longer than this.
constexpr size_t kMinCompressBytes = 20;

// Encodes records into a buffer and writes it to the file in blocks, keeping
// the CRC64 of everything written.
class Encoder {
 public:
  Encoder(int fd, const WriteOptions& options, aof::FileOps* file_ops)
      : fd_(fd), options_(&options), file_ops_(file_ops) {
    buffer_.reserve(kBlockBytes + 64);
  }

  [[nodiscard]] WriteStatus Status() const { return status_; }

  void Byte(uint8_t byte) {
    buffer_.push_back(static_cast<char>(byte));
    MaybeFlush();
  }

  // Redis' rdbSaveLen.
  void Length(uint64_t length) {
    if (length < (1U << 6)) {
      Byte(static_cast<uint8_t>(length | (kLength6Bit << 6)));
    } else if (length < (1U << 14)) {
      Byte(static_cast<uint8_t>((length >> 8) | (kLength14Bit << 6)));
      Byte(static_cast<uint8_t>(length & 0xff));
    } else if (length <= UINT32_MAX) {
      Byte(kLength32Bit);
      BigEndian(length, 4);
    } else {
      Byte(kLength64Bit);
      BigEndian(length, 8);
    }
  }

  // An 8-byte little-endian integer (Redis' rdbSaveMillisecondTime).
  void SignedInteger(int64_t value) {
    LittleEndian(static_cast<uint64_t>(value), 8);
  }

  // Redis' rdbSaveBinaryDoubleValue: the IEEE 754 bits, little-endian.
  void BinaryDouble(double value) {
    LittleEndian(std::bit_cast<uint64_t>(value), 8);
  }

  // Redis' rdbSaveLongLongAsStringObject: a small integer encoding when the
  // value fits 32 bits, otherwise its decimal text.
  void IntegerString(int64_t value) {
    if (EncodeInteger(value)) {
      return;
    }
    std::array<char, 21> text{};
    const int length = utils::Int64ToString(text, value);
    Length(static_cast<uint64_t>(length));
    Raw(std::string_view(text.data(), static_cast<size_t>(length)));
  }

  // Redis' rdbSaveRawString: an integer encoding for short decimal text, LZF
  // when it saves space, otherwise the length and the bytes.
  void String(std::string_view value) {
    int64_t integer = 0;
    if (value.size() <= 11 && utils::ToInt64(value, &integer) &&
        EncodeInteger(integer)) {
      return;
    }
    if (options_->compression && value.size() > kMinCompressBytes &&
        Compressed(value)) {
      return;
    }
    Length(value.size());
    Raw(value);
  }

  void Raw(std::string_view bytes) {
    if (bytes.size() < kDirectWriteBytes) {
      buffer_.append(bytes);
      MaybeFlush();
      return;
    }
    Flush();
    UpdateCrc(bytes);
    Write(bytes);
  }

  // Writes what is buffered, then the checksum: the CRC64 of everything
  // before it, or zero without rdbchecksum.
  void Finish() {
    UpdateCrc(buffer_);
    LittleEndianUnchecked(crc_, 8);
    Write(buffer_);
    buffer_.clear();
  }

 private:
  // Redis' rdbEncodeInteger: INT8, INT16, or INT32, little-endian.
  bool EncodeInteger(int64_t value) {
    if (value >= INT8_MIN && value <= INT8_MAX) {
      Byte((kEncodedValue << 6) | kEncodingInt8);
      LittleEndian(static_cast<uint64_t>(value), 1);
    } else if (value >= INT16_MIN && value <= INT16_MAX) {
      Byte((kEncodedValue << 6) | kEncodingInt16);
      LittleEndian(static_cast<uint64_t>(value), 2);
    } else if (value >= INT32_MIN && value <= INT32_MAX) {
      Byte((kEncodedValue << 6) | kEncodingInt32);
      LittleEndian(static_cast<uint64_t>(value), 4);
    } else {
      return false;
    }
    return true;
  }

  // Redis' rdbSaveLzfStringObject: only when it saves at least four bytes.
  bool Compressed(std::string_view value) {
    compressed_.resize(value.size() - 4);
    const size_t compressed_size = compressor_.Compress(value, compressed_);
    if (compressed_size == 0) {
      return false;
    }
    Byte((kEncodedValue << 6) | kEncodingLzf);
    Length(compressed_size);
    Length(value.size());
    Raw(std::string_view(compressed_.data(), compressed_size));
    return true;
  }

  void BigEndian(uint64_t value, size_t width) {
    for (size_t i = width; i > 0; --i) {
      buffer_.push_back(static_cast<char>((value >> (8 * (i - 1))) & 0xff));
    }
    MaybeFlush();
  }

  void LittleEndian(uint64_t value, size_t width) {
    LittleEndianUnchecked(value, width);
    MaybeFlush();
  }

  void LittleEndianUnchecked(uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
      buffer_.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    }
  }

  void MaybeFlush() {
    if (buffer_.size() >= kBlockBytes) {
      Flush();
    }
  }

  void Flush() {
    UpdateCrc(buffer_);
    Write(buffer_);
    buffer_.clear();
  }

  // Without rdbchecksum the CRC stays zero, which the file then ends with.
  void UpdateCrc(std::string_view bytes) {
    if (options_->checksum) {
      crc_ = utils::Crc64(crc_, bytes);
    }
  }

  void Write(std::string_view bytes) {
    if (status_ != WriteStatus::kWritten || bytes.empty()) {
      return;
    }
    if (aof::WriteAll(file_ops_, fd_, bytes) !=
        static_cast<ssize_t>(bytes.size())) {
      status_ = WriteStatus::kWriteFailed;
      return;
    }
    unsynced_bytes_ += bytes.size();
    if (options_->incremental_fsync && unsynced_bytes_ >= kAutoSyncBytes) {
      unsynced_bytes_ = 0;
      if (!aof::SyncFile(file_ops_, fd_)) {
        status_ = WriteStatus::kSyncFailed;
      }
    }
  }

  int fd_;
  const WriteOptions* options_;
  aof::FileOps* file_ops_;
  std::string buffer_;
  uint64_t crc_{};
  size_t unsynced_bytes_{};
  WriteStatus status_{WriteStatus::kWritten};
  utils::LzfCompressor compressor_;
  std::vector<char> compressed_;
};

void AuxField(std::string_view key, std::string_view value, Encoder* out) {
  out->Byte(kOpcodeAux);
  out->String(key);
  out->String(value);
}

void AuxField(std::string_view key, int64_t value, Encoder* out) {
  std::array<char, 21> text{};
  const int length = utils::Int64ToString(text, value);
  AuxField(key, std::string_view(text.data(), static_cast<size_t>(length)),
           out);
}

// Redis' rdbSaveObjectType followed by rdbSaveStringObject(key) and
// rdbSaveObject.
void WriteKeyValue(std::string_view key, const db::RedisObject& object,
                   Encoder* out) {
  using ObjectType = db::RedisObject::ObjectType;
  switch (object.Type()) {
    case ObjectType::kString: {
      out->Byte(kTypeString);
      out->String(key);
      const std::optional<int64_t> integer =
          object.StringEncoding() == db::RedisObject::Encoding::kInt
              ? object.Int()
              : std::nullopt;
      if (integer.has_value()) {
        out->IntegerString(*integer);
      } else {
        out->String(object.String());
      }
      return;
    }
    case ObjectType::kList: {
      // Every list is a quicklist of listpack nodes in the file; a
      // listpack-encoded list is a single node.
      const data_types::List* list = object.List();
      out->Byte(kTypeListQuicklist2);
      out->String(key);
      out->Length(list->Encoding() == data_types::List::Encoding::kListpack
                      ? 1
                      : list->NodeCount());
      list->ForEachListpack([out](std::string_view node) {
        out->Length(kContainerPacked);
        out->String(node);
        return true;
      });
      return;
    }
    case ObjectType::kSet: {
      const data_types::Set* set = object.Set();
      switch (set->Encoding()) {
        case data_types::Set::Encoding::kIntset: {
          out->Byte(kTypeSetIntset);
          out->String(key);
          std::string blob;
          set->AppendIntsetBlob(&blob);
          out->String(blob);
          return;
        }
        case data_types::Set::Encoding::kListpack:
          out->Byte(kTypeSetListpack);
          out->String(key);
          out->String(set->ListpackBytes());
          return;
        case data_types::Set::Encoding::kDict:
          out->Byte(kTypeSet);
          out->String(key);
          out->Length(set->Size());
          set->ForEachMember([out](std::string_view member) {
            out->String(member);
            return true;
          });
          return;
      }
      return;
    }
    case ObjectType::kZSet: {
      const data_types::ZSet* zset = object.ZSet();
      if (const std::optional<std::string_view> listpack =
              zset->ListpackBytes()) {
        out->Byte(kTypeZSetListpack);
        out->String(key);
        out->String(*listpack);
        return;
      }
      // Redis writes members from the greatest score down, since its
      // skiplist inserts those at the head in O(1) when loading; any order
      // loads correctly.
      out->Byte(kTypeZSet2);
      out->String(key);
      out->Length(zset->Size());
      zset->ForEachEntry([out](std::string_view member, double score) {
        out->String(member);
        out->BinaryDouble(score);
        return true;
      });
      return;
    }
    case ObjectType::kHash: {
      const data_types::Hash* hash = object.Hash();
      if (hash->Encoding() == data_types::Hash::Encoding::kListpack) {
        out->Byte(kTypeHashListpack);
        out->String(key);
        out->String(hash->ListpackBytes());
        return;
      }
      out->Byte(kTypeHash);
      out->String(key);
      out->Length(hash->Size());
      hash->ForEachEntry([out](std::string_view field, std::string_view value) {
        out->String(field);
        out->String(value);
        return true;
      });
      return;
    }
  }
}
}  // namespace

WriteStatus WriteRdbFile(db::RedisDb* const db, const std::string& path,
                         const WriteOptions& options,
                         aof::FileOps* const file_ops) {
  const int fd = aof::OpenFile(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return WriteStatus::kWriteFailed;
  }
  Encoder out(fd, options, file_ops);
  // "REDIS" and the version as four digits.
  std::string magic(kMagic);
  for (int divisor = 1000; divisor > 0; divisor /= 10) {
    magic.push_back(static_cast<char>('0' + ((kWriteVersion / divisor) % 10)));
  }
  out.Raw(magic);
  // Redis' rdbSaveInfoAuxFields, in its order. used-mem only informs
  // Redis' loading progress, and this server does not track it.
  AuxField("redis-ver", kServerVersion, &out);
  AuxField("redis-bits", static_cast<int64_t>(sizeof(void*) * 8), &out);
  AuxField("ctime", options.creation_time, &out);
  AuxField("used-mem", int64_t{0}, &out);
  AuxField("aof-base", int64_t{options.aof_base ? 1 : 0}, &out);
  if (db->KeyCount() > 0) {
    out.Byte(kOpcodeSelectDb);
    out.Length(0);
    out.Byte(kOpcodeResizeDb);
    out.Length(db->KeyCount());
    out.Length(db->KeysWithExpirationCount());
    db->ForEachObject(
        [db, &out](std::string_view key, const db::RedisObject& object) {
          if (const std::optional<int64_t> expire = db->Expiration(key)) {
            out.Byte(kOpcodeExpireTimeMs);
            out.SignedInteger(*expire);
          }
          WriteKeyValue(key, object, &out);
          return out.Status() == WriteStatus::kWritten;
        });
  }
  out.Byte(kOpcodeEof);
  out.Finish();
  WriteStatus status = out.Status();
  if (status == WriteStatus::kWritten && !aof::SyncFile(file_ops, fd)) {
    status = WriteStatus::kSyncFailed;
  }
  if (close(fd) < 0 && status == WriteStatus::kWritten) {
    status = WriteStatus::kWriteFailed;
  }
  if (status != WriteStatus::kWritten) {
    unlink(path.c_str());
  }
  return status;
}
}  // namespace redis_simple::rdb
