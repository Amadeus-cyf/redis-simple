#include "server/rdb/rdb_loader.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "containers/listpack.h"
#include "containers/listpack_reader.h"
#include "data_types/hash/hash.h"
#include "data_types/list/list.h"
#include "data_types/set/set.h"
#include "data_types/zset/zset.h"
#include "logging/logger.h"
#include "server/aof/file_ops.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/rdb/rdb_format.h"
#include "utils/crc64.h"
#include "utils/floats.h"
#include "utils/lzf.h"
#include "utils/strings.h"

namespace redis_simple::rdb {
namespace {
// The largest string this server holds, its proto-max-bulk-len. It bounds
// what an LZF-compressed string may claim to expand to.
constexpr size_t kMaxStringBytes = size_t{512} * 1024 * 1024;
constexpr size_t kReadBufferBytes = size_t{64} * 1024;

// Reads the file through a buffer, keeping the CRC64 of what was read and
// how much of the file remains, which bounds every length the file claims.
class Input {
 public:
  Input(int fd, size_t remaining) : fd_(fd), remaining_(remaining) {}

  [[nodiscard]] size_t Consumed() const { return consumed_; }
  [[nodiscard]] size_t Remaining() const { return remaining_ - consumed_; }
  [[nodiscard]] uint64_t Crc() const { return crc_; }

  bool Read(void* const out, size_t size, bool update_crc = true) {
    auto* destination = static_cast<char*>(out);
    if (size > Remaining()) {
      return false;
    }
    while (size > 0) {
      if (position_ == filled_ && !Fill()) {
        return false;
      }
      const size_t chunk = std::min(size, filled_ - position_);
      const std::string_view bytes(buffer_.data() + position_, chunk);
      if (update_crc) {
        crc_ = utils::Crc64(crc_, bytes);
      }
      std::memcpy(destination, bytes.data(), chunk);
      destination += chunk;
      position_ += chunk;
      consumed_ += chunk;
      size -= chunk;
    }
    return true;
  }

 private:
  bool Fill() {
    while (true) {
      const ssize_t result = read(fd_, buffer_.data(), buffer_.size());
      if (result > 0) {
        position_ = 0;
        filled_ = static_cast<size_t>(result);
        return true;
      }
      if (result == 0 || errno != EINTR) {
        return false;
      }
    }
  }

  int fd_;
  size_t remaining_;
  size_t consumed_{};
  uint64_t crc_{};
  std::array<char, kReadBufferBytes> buffer_{};
  size_t position_{};
  size_t filled_{};
};

// Why reading a value stopped.
enum class ValueStatus : uint8_t {
  kRead,
  // A collection with no elements, which Redis skips.
  kEmpty,
  kFailed,
};

std::string_view TypeName(int type) {
  switch (type) {
    case kTypeModulePreGa:
    case kTypeModule2:
      return "a module type";
    case kTypeHashZipmap:
    case kTypeListZiplist:
    case kTypeZSetZiplist:
    case kTypeHashZiplist:
    case kTypeListQuicklist:
      return "a ziplist encoding (Redis 6.2 and earlier)";
    case kTypeStreamListpacks:
    case kTypeStreamListpacks2:
    case kTypeStreamListpacks3:
    case kTypeStreamListpacks4:
    case kTypeStreamListpacks5:
      return "a stream";
    case kTypeHashMetadataPreGa:
    case kTypeHashListpackExPreGa:
    case kTypeHashMetadata:
    case kTypeHashListpackEx:
      return "a hash with field expiration";
    case kTypeHashTemplateListpack:
    case kTypeHashTemplateListpackRef:
    case kTypeHashTemplateArray:
    case kTypeHashTemplateArrayRef:
      return "a hash template";
    case kTypeArray:
      return "an array";
    case kTypeGcra:
      return "a GCRA rate limiter";
    default:
      return "an unknown type";
  }
}

// What a set, hash, or sorted set listpack from the file can become.
enum class CompactCheck : uint8_t {
  // Valid and within this server's listpack limits: keep its bytes.
  kAdopt,
  // Valid, but too large for a listpack here or encoded differently from
  // this server's listpacks: insert its elements one by one.
  kRebuild,
  kCorrupt,
};

// A listpack-encoded type's layout and limits.
struct CompactKind {
  std::string_view name;
  // What its keys are called: members or fields.
  std::string_view keys;
  // 1 for a set; 2 for the pairs of a hash or sorted set.
  size_t elements_per_entry;
  size_t max_entries;
  size_t max_element_length;
  bool sorted_set;
};

constexpr CompactKind kSetKind{
    .name = "set",
    .keys = "members",
    .elements_per_entry = 1,
    .max_entries = data_types::Set::kListpackMaxEntries,
    .max_element_length = data_types::Set::kListpackMaxElementLength,
    .sorted_set = false};
constexpr CompactKind kHashKind{
    .name = "hash",
    .keys = "fields",
    .elements_per_entry = 2,
    .max_entries = data_types::Hash::kListpackMaxEntries,
    .max_element_length = data_types::Hash::kListpackMaxElementLength,
    .sorted_set = false};
constexpr CompactKind kZSetKind{
    .name = "sorted set",
    .keys = "members",
    .elements_per_entry = 2,
    .max_entries = data_types::ZSet::kListpackMaxEntries,
    .max_element_length = data_types::ZSet::kListpackMaxElementLength,
    .sorted_set = true};

class Loader {
 public:
  Loader(int fd, size_t remaining, db::RedisDb* db, const LoadOptions& options,
         LoadStats* stats)
      : input_(fd, remaining), db_(db), options_(&options), stats_(stats) {}

  bool Load();

 private:
  // Logs a corruption at the current offset, as Redis' rdbReportCorruptRDB
  // does, and returns false.
  bool Corrupt(std::string_view what) {
    RS_LOG_ERROR("Bad RDB data at offset %zu: %.*s\n", input_.Consumed(),
                 static_cast<int>(what.size()), what.data());
    return false;
  }

  bool ShortRead() {
    RS_LOG_ERROR(
        "Short read loading the RDB data at offset %zu: unexpected end of "
        "file\n",
        input_.Consumed());
    return false;
  }

  bool ReadByte(uint8_t* const byte) {
    return input_.Read(byte, 1) || ShortRead();
  }

  bool ReadLittleEndian(size_t width, uint64_t* const value) {
    std::array<unsigned char, 8> bytes{};
    if (!input_.Read(bytes.data(), width)) {
      return ShortRead();
    }
    *value = 0;
    for (size_t i = 0; i < width; ++i) {
      *value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return true;
  }

  // Redis' rdbLoadLenByRef. `*encoded` reports a special string encoding,
  // whose type is then in `*length`.
  bool ReadLength(uint64_t* const length, bool* const encoded = nullptr) {
    uint8_t first = 0;
    if (!ReadByte(&first)) {
      return false;
    }
    if (encoded != nullptr) {
      *encoded = false;
    }
    const unsigned kind = (first & 0xc0) >> 6;
    if (kind == kEncodedValue) {
      if (encoded == nullptr) {
        return Corrupt("a string encoding where a length was expected");
      }
      *encoded = true;
      *length = first & 0x3f;
      return true;
    }
    if (kind == kLength6Bit) {
      *length = first & 0x3f;
      return true;
    }
    if (kind == kLength14Bit) {
      uint8_t second = 0;
      if (!ReadByte(&second)) {
        return false;
      }
      *length = (static_cast<uint64_t>(first & 0x3f) << 8) | second;
      return true;
    }
    if (first != kLength32Bit && first != kLength64Bit) {
      return Corrupt("an unknown length encoding");
    }
    const size_t width = first == kLength32Bit ? 4 : 8;
    std::array<unsigned char, 8> bytes{};
    if (!input_.Read(bytes.data(), width)) {
      return ShortRead();
    }
    *length = 0;
    for (size_t i = 0; i < width; ++i) {
      *length = (*length << 8) | bytes[i];
    }
    return true;
  }

  // A collection's element count, which can be no larger than the bytes
  // left, since each element takes at least one.
  bool ReadCount(uint64_t* const count) {
    if (!ReadLength(count)) {
      return false;
    }
    if (*count > input_.Remaining()) {
      return Corrupt("an element count larger than the rest of the file");
    }
    return true;
  }

  bool ReadBytes(size_t size, std::string* const out) {
    if (size > input_.Remaining()) {
      return Corrupt("a string longer than the rest of the file");
    }
    out->resize(size);
    return input_.Read(out->data(), size) || ShortRead();
  }

  // Redis' rdbGenericLoadStringObject: an integer encoding, LZF, or plain.
  bool ReadString(std::string* const out) {
    uint64_t length = 0;
    bool encoded = false;
    if (!ReadLength(&length, &encoded)) {
      return false;
    }
    if (!encoded) {
      return ReadBytes(length, out);
    }
    switch (length) {
      case kEncodingInt8:
      case kEncodingInt16:
      case kEncodingInt32: {
        const size_t width = size_t{1} << length;
        uint64_t raw = 0;
        if (!ReadLittleEndian(width, &raw)) {
          return false;
        }
        // Sign-extend from the encoded width.
        const unsigned shift = 64 - (8 * static_cast<unsigned>(width));
        const int64_t value = static_cast<int64_t>(raw << shift) >> shift;
        std::array<char, 21> text{};
        const int text_length = utils::Int64ToString(text, value);
        out->assign(text.data(), static_cast<size_t>(text_length));
        return true;
      }
      case kEncodingLzf:
        return ReadCompressedString(out);
      default:
        return Corrupt("an unknown string encoding");
    }
  }

  // Redis' rdbLoadLzfStringObject.
  bool ReadCompressedString(std::string* const out) {
    uint64_t compressed_length = 0;
    uint64_t length = 0;
    if (!ReadLength(&compressed_length) || !ReadLength(&length)) {
      return false;
    }
    if (length == 0 || length > kMaxStringBytes) {
      return Corrupt("an invalid uncompressed string length");
    }
    if (!ReadBytes(compressed_length, &compressed_)) {
      return false;
    }
    out->resize(length);
    if (!utils::LzfDecompress(compressed_, *out)) {
      return Corrupt("an invalid LZF-compressed string");
    }
    return true;
  }

  bool ReadBinaryDouble(double* const value) {
    uint64_t bits = 0;
    if (!ReadLittleEndian(8, &bits)) {
      return false;
    }
    *value = std::bit_cast<double>(bits);
    return true;
  }

  // Redis' rdbLoadDoubleValue: a length byte, where 253 to 255 mean NaN,
  // +inf, and -inf, then that much text. Only the old ZSET type uses it.
  bool ReadTextDouble(double* const value) {
    uint8_t length = 0;
    if (!ReadByte(&length)) {
      return false;
    }
    switch (length) {
      case 255:
        *value = -std::numeric_limits<double>::infinity();
        return true;
      case 254:
        *value = std::numeric_limits<double>::infinity();
        return true;
      case 253:
        *value = std::numeric_limits<double>::quiet_NaN();
        return true;
      default:
        break;
    }
    std::string text;
    if (!ReadBytes(length, &text)) {
      return false;
    }
    const std::optional<double> parsed = utils::ToRangeDouble(text);
    if (!parsed.has_value()) {
      return Corrupt("an invalid score");
    }
    *value = *parsed;
    return true;
  }

  CompactCheck CheckCompactListpack(std::string_view blob,
                                    const CompactKind& kind);
  ValueStatus ReadValue(int type, db::RedisObjectPtr* out);
  ValueStatus ReadList(int type, db::RedisObjectPtr* out);
  ValueStatus ReadSet(int type, db::RedisObjectPtr* out);
  ValueStatus ReadZSet(int type, db::RedisObjectPtr* out);
  ValueStatus ReadHash(int type, db::RedisObjectPtr* out);
  bool ReadIntset(const std::string& blob, data_types::Set* set);
  bool SkipAux();

  Input input_;
  db::RedisDb* db_;
  const LoadOptions* options_;
  LoadStats* stats_;
  std::string compressed_;
  // CheckCompactListpack's duplicate checks, kept to reuse their capacity.
  std::vector<std::string_view> strings_;
  std::vector<int64_t> integers_;
};

// The text of a listpack element, formatted into `buffer` for an integer.
std::string_view ElementText(const containers::ListpackReader::Element& element,
                             std::array<char, 21>* const buffer) {
  if (!element.is_integer) {
    return element.string;
  }
  const int length = utils::Int64ToString(*buffer, element.integer);
  return {buffer->data(), static_cast<size_t>(length)};
}

// As Redis' lpValidateIntegrityAndDups does for a set, hash, or sorted set
// listpack, checks the encoding, the pairing, and that keys are distinct,
// and that sorted set scores are numbers. Redis then keeps the listpack's
// bytes, which turns loading into a copy; so does this server when the
// listpack also fits its own limits, has its sorted set pairs in order, and
// stores each element as Listpack would (an integer exactly when its text
// parses as one), since lookups depend on that.
CompactCheck Loader::CheckCompactListpack(std::string_view blob,
                                          const CompactKind& kind) {
  strings_.clear();
  integers_.clear();
  containers::ListpackReader reader(blob);
  containers::ListpackReader::Element element;
  containers::ListpackReader::Status status =
      containers::ListpackReader::Status::kEnd;
  bool fits = true;
  size_t count = 0;
  // A sorted set's previous and current members as text; integer members
  // are formatted into alternating buffers so the previous one survives.
  std::array<std::array<char, 21>, 2> buffers{};
  size_t buffer_index = 0;
  std::string_view member;
  std::string_view previous_member;
  double previous_score = 0;
  while ((status = reader.Next(&element)) ==
         containers::ListpackReader::Status::kElement) {
    const bool is_key = count % kind.elements_per_entry == 0;
    ++count;
    if (!element.is_integer) {
      if (element.string.size() > kind.max_element_length ||
          utils::ToInt64(element.string).has_value()) {
        fits = false;
      }
    }
    if (is_key) {
      if (element.is_integer) {
        integers_.push_back(element.integer);
      } else {
        strings_.push_back(element.string);
      }
      if (kind.sorted_set) {
        member = ElementText(element, &buffers[buffer_index]);
      }
      continue;
    }
    if (!kind.sorted_set) {
      continue;
    }
    const std::optional<double> parsed_score =
        element.is_integer ? static_cast<double>(element.integer)
                           : utils::ToDouble(element.string);
    if (!parsed_score.has_value()) {
      Corrupt("an invalid sorted set score");
      return CompactCheck::kCorrupt;
    }
    const double score = *parsed_score;
    if (std::isnan(score)) {
      Corrupt("a sorted set score that is NaN");
      return CompactCheck::kCorrupt;
    }
    // Out of order pairs are inserted one by one, which sorts them.
    if (count > 2 && (score < previous_score ||
                      (score == previous_score && member <= previous_member))) {
      fits = false;
    }
    previous_score = score;
    previous_member = member;
    buffer_index ^= 1;
  }
  if (status == containers::ListpackReader::Status::kCorrupt ||
      count % kind.elements_per_entry != 0) {
    Corrupt(std::string("a ") + std::string(kind.name) +
            " listpack failed its integrity check");
    return CompactCheck::kCorrupt;
  }
  const size_t entries = count / kind.elements_per_entry;
  if (!fits || entries == 0 || entries > kind.max_entries) {
    return CompactCheck::kRebuild;
  }
  std::ranges::sort(strings_);
  std::ranges::sort(integers_);
  if (std::ranges::adjacent_find(strings_) != strings_.end() ||
      std::ranges::adjacent_find(integers_) != integers_.end()) {
    Corrupt(std::string("duplicate ") + std::string(kind.name) + " " +
            std::string(kind.keys));
    return CompactCheck::kCorrupt;
  }
  return CompactCheck::kAdopt;
}

ValueStatus Loader::ReadList(int type, db::RedisObjectPtr* const out) {
  uint64_t count = 0;
  if (!ReadCount(&count)) {
    return ValueStatus::kFailed;
  }
  std::unique_ptr<data_types::List> list = data_types::List::Create();
  std::string element;
  if (type == kTypeList) {
    for (; count > 0; --count) {
      if (!ReadString(&element)) {
        return ValueStatus::kFailed;
      }
      list->RPush(element);
    }
  } else {
    // A quicklist: `count` nodes, each a listpack or one plain element.
    std::string node;
    std::array<char, 21> buffer{};
    for (; count > 0; --count) {
      uint64_t container = 0;
      if (!ReadLength(&container)) {
        return ValueStatus::kFailed;
      }
      if (container != kContainerPacked && container != kContainerPlain) {
        Corrupt("an unknown quicklist node container");
        return ValueStatus::kFailed;
      }
      if (!ReadString(&node)) {
        return ValueStatus::kFailed;
      }
      if (node.empty()) {
        Corrupt("an empty quicklist node");
        return ValueStatus::kFailed;
      }
      if (container == kContainerPlain) {
        list->RPush(node);
        continue;
      }
      containers::ListpackReader reader(node);
      containers::ListpackReader::Element listpack_element;
      containers::ListpackReader::Status status =
          containers::ListpackReader::Status::kEnd;
      while ((status = reader.Next(&listpack_element)) ==
             containers::ListpackReader::Status::kElement) {
        list->RPush(ElementText(listpack_element, &buffer));
      }
      if (status == containers::ListpackReader::Status::kCorrupt) {
        Corrupt("a list listpack failed its integrity check");
        return ValueStatus::kFailed;
      }
    }
  }
  if (list->Size() == 0) {
    return ValueStatus::kEmpty;
  }
  *out = db::RedisObject::CreateWithList(std::move(list));
  return ValueStatus::kRead;
}

// Redis' intsetValidateIntegrity with deep validation: a known width, a
// length matching the blob, and values in strictly ascending order.
bool Loader::ReadIntset(const std::string& blob, data_types::Set* const set) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(blob.data());
  const auto load = [bytes](size_t offset, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) {
      value |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
  };
  if (blob.size() < 8) {
    return Corrupt("an intset shorter than its header");
  }
  const auto width = static_cast<size_t>(load(0, 4));
  const uint64_t length = load(4, 4);
  if ((width != 2 && width != 4 && width != 8) ||
      length != (blob.size() - 8) / width || (blob.size() - 8) % width != 0) {
    return Corrupt("an intset failed its integrity check");
  }
  std::optional<int64_t> previous;
  const unsigned shift = 64 - (8 * static_cast<unsigned>(width));
  std::array<char, 21> text{};
  for (uint64_t index = 0; index < length; ++index) {
    const int64_t value =
        static_cast<int64_t>(load(8 + (index * width), width) << shift) >>
        shift;
    if (previous.has_value() && value <= *previous) {
      return Corrupt("an intset that is not in ascending order");
    }
    previous = value;
    const int text_length = utils::Int64ToString(text, value);
    set->Add(std::string_view(text.data(), static_cast<size_t>(text_length)));
  }
  return true;
}

ValueStatus Loader::ReadSet(int type, db::RedisObjectPtr* const out) {
  std::unique_ptr<data_types::Set> set = data_types::Set::Create();
  std::string element;
  if (type == kTypeSet) {
    uint64_t count = 0;
    if (!ReadCount(&count)) {
      return ValueStatus::kFailed;
    }
    set = data_types::Set::CreateForSize(count);
    for (; count > 0; --count) {
      if (!ReadString(&element)) {
        return ValueStatus::kFailed;
      }
      if (!set->Add(element)) {
        Corrupt("duplicate set members");
        return ValueStatus::kFailed;
      }
    }
  } else if (type == kTypeSetIntset) {
    if (!ReadString(&element) || !ReadIntset(element, set.get())) {
      return ValueStatus::kFailed;
    }
  } else {
    if (!ReadString(&element)) {
      return ValueStatus::kFailed;
    }
    switch (CheckCompactListpack(element, kSetKind)) {
      case CompactCheck::kAdopt:
        *out =
            db::RedisObject::CreateWithSet(data_types::Set::CreateFromListpack(
                containers::Listpack::CreateFromBytes(element)));
        return ValueStatus::kRead;
      case CompactCheck::kCorrupt:
        return ValueStatus::kFailed;
      case CompactCheck::kRebuild:
        break;
    }
    containers::ListpackReader reader(element);
    containers::ListpackReader::Element member;
    containers::ListpackReader::Status status =
        containers::ListpackReader::Status::kEnd;
    std::array<char, 21> buffer{};
    while ((status = reader.Next(&member)) ==
           containers::ListpackReader::Status::kElement) {
      if (!set->Add(ElementText(member, &buffer))) {
        Corrupt("duplicate set members");
        return ValueStatus::kFailed;
      }
    }
    if (status == containers::ListpackReader::Status::kCorrupt) {
      Corrupt("a set listpack failed its integrity check");
      return ValueStatus::kFailed;
    }
  }
  if (set->Size() == 0) {
    return ValueStatus::kEmpty;
  }
  *out = db::RedisObject::CreateWithSet(std::move(set));
  return ValueStatus::kRead;
}

ValueStatus Loader::ReadZSet(int type, db::RedisObjectPtr* const out) {
  std::unique_ptr<data_types::ZSet> zset = data_types::ZSet::Create();
  std::string member;
  const auto insert = [this, &zset](std::string_view member_view,
                                    double score) {
    if (std::isnan(score)) {
      return Corrupt("a sorted set score that is NaN");
    }
    if (!zset->InsertOrUpdate(member_view, score)) {
      return Corrupt("duplicate sorted set members");
    }
    return true;
  };
  if (type == kTypeZSet || type == kTypeZSet2) {
    uint64_t count = 0;
    if (!ReadCount(&count)) {
      return ValueStatus::kFailed;
    }
    zset = data_types::ZSet::CreateForSize(count);
    for (; count > 0; --count) {
      double score = 0;
      if (!ReadString(&member) ||
          !(type == kTypeZSet2 ? ReadBinaryDouble(&score)
                               : ReadTextDouble(&score)) ||
          !insert(member, score)) {
        return ValueStatus::kFailed;
      }
    }
  } else {
    // Listpack pairs: each member, then its score as Redis' d2string text,
    // which the listpack may store as an integer.
    std::string blob;
    if (!ReadString(&blob)) {
      return ValueStatus::kFailed;
    }
    switch (CheckCompactListpack(blob, kZSetKind)) {
      case CompactCheck::kAdopt:
        *out = db::RedisObject::CreateWithZSet(
            data_types::ZSet::CreateFromListpack(
                containers::Listpack::CreateFromBytes(blob)));
        return ValueStatus::kRead;
      case CompactCheck::kCorrupt:
        return ValueStatus::kFailed;
      case CompactCheck::kRebuild:
        break;
    }
    containers::ListpackReader reader(blob);
    containers::ListpackReader::Element element;
    containers::ListpackReader::Status status =
        containers::ListpackReader::Status::kEnd;
    std::array<char, 21> buffer{};
    bool expecting_score = false;
    while ((status = reader.Next(&element)) ==
           containers::ListpackReader::Status::kElement) {
      if (!expecting_score) {
        member.assign(ElementText(element, &buffer));
        expecting_score = true;
        continue;
      }
      expecting_score = false;
      const std::optional<double> score =
          element.is_integer ? static_cast<double>(element.integer)
                             : utils::ToDouble(element.string);
      if (!score.has_value()) {
        Corrupt("an invalid sorted set score");
        return ValueStatus::kFailed;
      }
      if (!insert(member, *score)) {
        return ValueStatus::kFailed;
      }
    }
    if (status == containers::ListpackReader::Status::kCorrupt ||
        expecting_score) {
      Corrupt("a sorted set listpack failed its integrity check");
      return ValueStatus::kFailed;
    }
  }
  if (zset->Size() == 0) {
    return ValueStatus::kEmpty;
  }
  *out = db::RedisObject::CreateWithZSet(std::move(zset));
  return ValueStatus::kRead;
}

ValueStatus Loader::ReadHash(int type, db::RedisObjectPtr* const out) {
  std::unique_ptr<data_types::Hash> hash = data_types::Hash::Create();
  std::string field;
  std::string value;
  if (type == kTypeHash) {
    uint64_t count = 0;
    if (!ReadCount(&count)) {
      return ValueStatus::kFailed;
    }
    hash = data_types::Hash::CreateForSize(count);
    for (; count > 0; --count) {
      if (!ReadString(&field) || !ReadString(&value)) {
        return ValueStatus::kFailed;
      }
      if (!hash->Set(field, value)) {
        Corrupt("duplicate hash fields");
        return ValueStatus::kFailed;
      }
    }
  } else {
    std::string blob;
    if (!ReadString(&blob)) {
      return ValueStatus::kFailed;
    }
    switch (CheckCompactListpack(blob, kHashKind)) {
      case CompactCheck::kAdopt:
        *out = db::RedisObject::CreateWithHash(
            data_types::Hash::CreateFromListpack(
                containers::Listpack::CreateFromBytes(blob)));
        return ValueStatus::kRead;
      case CompactCheck::kCorrupt:
        return ValueStatus::kFailed;
      case CompactCheck::kRebuild:
        break;
    }
    containers::ListpackReader reader(blob);
    containers::ListpackReader::Element element;
    containers::ListpackReader::Status status =
        containers::ListpackReader::Status::kEnd;
    std::array<char, 21> buffer{};
    bool expecting_value = false;
    while ((status = reader.Next(&element)) ==
           containers::ListpackReader::Status::kElement) {
      if (!expecting_value) {
        field.assign(ElementText(element, &buffer));
        expecting_value = true;
        continue;
      }
      expecting_value = false;
      if (!hash->Set(field, ElementText(element, &buffer))) {
        Corrupt("duplicate hash fields");
        return ValueStatus::kFailed;
      }
    }
    if (status == containers::ListpackReader::Status::kCorrupt ||
        expecting_value) {
      Corrupt("a hash listpack failed its integrity check");
      return ValueStatus::kFailed;
    }
  }
  if (hash->Size() == 0) {
    return ValueStatus::kEmpty;
  }
  *out = db::RedisObject::CreateWithHash(std::move(hash));
  return ValueStatus::kRead;
}

ValueStatus Loader::ReadValue(int type, db::RedisObjectPtr* const out) {
  switch (type) {
    case kTypeString: {
      std::string value;
      if (!ReadString(&value)) {
        return ValueStatus::kFailed;
      }
      *out = db::RedisObject::CreateWithString(std::move(value));
      return ValueStatus::kRead;
    }
    case kTypeList:
    case kTypeListQuicklist2:
      return ReadList(type, out);
    case kTypeSet:
    case kTypeSetIntset:
    case kTypeSetListpack:
      return ReadSet(type, out);
    case kTypeZSet:
    case kTypeZSet2:
    case kTypeZSetListpack:
      return ReadZSet(type, out);
    case kTypeHash:
    case kTypeHashListpack:
      return ReadHash(type, out);
    default: {
      const std::string_view name = TypeName(type);
      RS_LOG_ERROR(
          "The RDB data holds %.*s (RDB type %d), which this server does not "
          "support\n",
          static_cast<int>(name.size()), name.data(), type);
      return ValueStatus::kFailed;
    }
  }
}

// An aux field: a key and a value. Redis logs a few; none change loading.
bool Loader::SkipAux() {
  std::string key;
  std::string value;
  if (!ReadString(&key) || !ReadString(&value)) {
    return false;
  }
  if (key == "redis-ver") {
    RS_LOG_INFO("Loading RDB produced by version %s\n", value.c_str());
  }
  return true;
}

bool Loader::Load() {
  std::array<char, 9> header{};
  if (!input_.Read(header.data(), header.size())) {
    return ShortRead();
  }
  const std::string_view header_view(header.data(), header.size());
  if (!header_view.starts_with(kMagic)) {
    RS_LOG_ERROR("Wrong signature trying to load DB from file\n");
    return false;
  }
  // The version: four digits, such as "0011".
  int version = 0;
  for (const char c : header_view.substr(kMagic.size())) {
    if (c < '0' || c > '9') {
      version = -1;
      break;
    }
    version = (version * 10) + (c - '0');
  }
  if (version < 1 || version > kMaxLoadVersion) {
    RS_LOG_ERROR("Can't handle RDB format version %d\n", version);
    return false;
  }

  std::optional<int64_t> expire;
  std::string key;
  while (true) {
    uint8_t type = 0;
    if (!ReadByte(&type)) {
      return false;
    }
    switch (type) {
      case kOpcodeExpireTime: {
        uint64_t seconds = 0;
        if (!ReadLittleEndian(4, &seconds)) {
          return false;
        }
        expire = static_cast<int64_t>(static_cast<int32_t>(seconds)) * 1000;
        continue;
      }
      case kOpcodeExpireTimeMs: {
        uint64_t milliseconds = 0;
        if (!ReadLittleEndian(8, &milliseconds)) {
          return false;
        }
        expire = static_cast<int64_t>(milliseconds);
        continue;
      }
      case kOpcodeFreq: {
        // LFU frequency, for maxmemory policies this server does not have.
        uint8_t frequency = 0;
        if (!ReadByte(&frequency)) {
          return false;
        }
        continue;
      }
      case kOpcodeIdle: {
        uint64_t idle = 0;
        if (!ReadLength(&idle)) {
          return false;
        }
        continue;
      }
      case kOpcodeEof:
        break;
      case kOpcodeSelectDb: {
        uint64_t database = 0;
        if (!ReadLength(&database)) {
          return false;
        }
        if (database != 0) {
          RS_LOG_ERROR("The RDB data holds keys in database %" PRIu64
                       "; this server has only "
                       "database 0\n",
                       database);
          return false;
        }
        continue;
      }
      case kOpcodeResizeDb: {
        // A sizing hint: the key count and the count with expirations.
        uint64_t key_count = 0;
        uint64_t expire_count = 0;
        if (!ReadLength(&key_count) || !ReadLength(&expire_count)) {
          return false;
        }
        continue;
      }
      case kOpcodeSlotInfo: {
        // Cluster slot sizes, which Redis ignores outside cluster mode.
        uint64_t ignored = 0;
        if (!ReadLength(&ignored) || !ReadLength(&ignored) ||
            !ReadLength(&ignored)) {
          return false;
        }
        continue;
      }
      case kOpcodeAux:
        if (!SkipAux()) {
          return false;
        }
        continue;
      case kOpcodeFunction2: {
        std::string code;
        if (!ReadString(&code)) {
          return false;
        }
        RS_LOG_WARN(
            "Skipping a function library in the RDB data: this server does "
            "not support functions\n");
        continue;
      }
      case kOpcodeModuleAux:
      case kOpcodeKeyMeta:
      case kOpcodeHashTemplate:
      case kOpcodeFunctionPreGa:
        RS_LOG_ERROR(
            "The RDB data holds module, key metadata, hash template, or "
            "pre-release function records (opcode %d), which this server does "
            "not support\n",
            type);
        return false;
      default: {
        if (!ReadString(&key)) {
          return false;
        }
        db::RedisObjectPtr value;
        switch (ReadValue(type, &value)) {
          case ValueStatus::kFailed:
            return false;
          case ValueStatus::kEmpty:
            ++stats_->empty_keys_skipped;
            expire.reset();
            continue;
          case ValueStatus::kRead:
            break;
        }
        if (!options_->keep_expired && expire.has_value() &&
            *expire < options_->now_ms) {
          ++stats_->keys_expired;
          expire.reset();
          continue;
        }
        if (db_->LookupKey(key) != nullptr) {
          return Corrupt("a duplicated key");
        }
        db_->SetKey(key, std::move(value), expire.value_or(0));
        ++stats_->keys_loaded;
        expire.reset();
        continue;
      }
    }
    break;
  }
  if (version >= 5) {
    const uint64_t expected = input_.Crc();
    uint64_t checksum = 0;
    std::array<unsigned char, 8> bytes{};
    if (!input_.Read(bytes.data(), bytes.size(), /*update_crc=*/false)) {
      return ShortRead();
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
      checksum |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    // Zero means the writer had rdbchecksum off.
    if (checksum != 0 && checksum != expected) {
      RS_LOG_ERROR("Wrong RDB checksum expected: (%" PRIx64
                   ") but got (%" PRIx64 "). Aborting now.\n",
                   expected, checksum);
      return false;
    }
  }
  stats_->bytes = input_.Consumed();
  return true;
}
}  // namespace

bool LoadRdb(int fd, db::RedisDb* const db, const LoadOptions& options,
             LoadStats* const stats) {
  const std::optional<size_t> size = aof::FileSize(fd);
  const off_t position = lseek(fd, 0, SEEK_CUR);
  if (!size.has_value() || position < 0 || std::cmp_greater(position, *size)) {
    RS_LOG_ERROR("Cannot read the RDB file's size\n");
    return false;
  }
  *stats = {};
  // Keys load as they were, even if logically expired, until loading ends.
  db->SetLoading(true);
  auto loader = std::make_unique<Loader>(
      fd, *size - static_cast<size_t>(position), db, options, stats);
  const bool loaded = loader->Load();
  db->SetLoading(false);
  return loaded;
}
}  // namespace redis_simple::rdb
