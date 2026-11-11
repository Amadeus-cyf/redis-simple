#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "data_types/hash/hash.h"
#include "data_types/list/list.h"
#include "data_types/set/set.h"
#include "data_types/zset/zset.h"
#include "fuzz/fuzz_input.h"
#include "server/aof/file_ops.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/rdb/rdb_loader.h"
#include "server/rdb/rdb_writer.h"
#include "utils/float_utils.h"

namespace redis_simple::fuzz {
namespace {
using db::RedisObject;

constexpr int64_t kFuture = int64_t{4102444800000};
constexpr size_t kMaxKeys = 8;
constexpr size_t kMaxElements = 160;

// A key's contents as text, independent of hash table order.
std::string Describe(const RedisObject& object) {
  std::string out;
  switch (object.Type()) {
    case RedisObject::ObjectType::kString:
      return "s:" + std::string(object.String().View());
    case RedisObject::ObjectType::kList: {
      const list::List* list = object.List();
      out = "l:";
      list->ForEach(0, list->Size() - 1, [&out](std::string_view value) {
        out.append(value).push_back('\0');
        return true;
      });
      return out;
    }
    case RedisObject::ObjectType::kSet: {
      std::vector<std::string> members;
      object.Set()->ForEachMember([&members](std::string_view member) {
        members.emplace_back(member);
        return true;
      });
      std::ranges::sort(members);
      out = "S:";
      for (const std::string& member : members) {
        out.append(member).push_back('\0');
      }
      return out;
    }
    case RedisObject::ObjectType::kZSet:
      out = "z:";
      object.ZSet()->ForEachEntry(
          [&out](std::string_view member, double score) {
            out.append(member).push_back('\0');
            out.append(utils::FloatToString(score)).push_back('\0');
            return true;
          });
      return out;
    case RedisObject::ObjectType::kHash: {
      std::vector<std::string> entries;
      object.Hash()->ForEachEntry(
          [&entries](std::string_view field, std::string_view value) {
            entries.push_back(std::string(field) + '\0' + std::string(value));
            return true;
          });
      std::ranges::sort(entries);
      out = "h:";
      for (const std::string& entry : entries) {
        out.append(entry).push_back('\0');
      }
      return out;
    }
  }
  return out;
}

std::map<std::string, std::string> Snapshot(db::RedisDb* db) {
  std::map<std::string, std::string> keys;
  db->ForEachObject(
      [db, &keys](std::string_view key, const RedisObject& object) {
        std::string description = Describe(object);
        if (const std::optional<int64_t> expire = db->Expiration(key)) {
          description += " @" + std::to_string(*expire);
        }
        keys.emplace(key, std::move(description));
        return true;
      });
  return keys;
}

// Element text: sometimes an integer, sometimes long enough to leave the
// compact encodings.
std::string ReadElement(FuzzInput* input) {
  switch (input->ReadByte() % 4) {
    case 0:
      return std::to_string(input->ReadInt64());
    case 1:
      return PadToSize(input->ReadValue(8), 70);
    default:
      return input->ReadValue(12);
  }
}

void Populate(FuzzInput* input, db::RedisDb* db) {
  const size_t key_count = 1 + input->ReadIndex(kMaxKeys);
  for (size_t index = 0; index < key_count && input->HasData(); ++index) {
    const std::string key = "k" + input->ReadValue(6);
    const size_t elements = 1 + input->ReadIndex(kMaxElements);
    db::RedisObjectPtr object;
    switch (input->ReadByte() % 5) {
      case 0:
        object = RedisObject::CreateWithString(
            input->ReadByte() % 2 == 0 ? ReadElement(input)
                                       : std::string(elements * 13, 'r'));
        break;
      case 1: {
        auto list = list::List::Create();
        for (size_t i = 0; i < elements; ++i) {
          list->RPush(ReadElement(input));
        }
        object = RedisObject::CreateWithList(std::move(list));
        break;
      }
      case 2: {
        auto set = set::Set::Create();
        for (size_t i = 0; i < elements; ++i) {
          set->Add(ReadElement(input));
        }
        object = RedisObject::CreateWithSet(std::move(set));
        break;
      }
      case 3: {
        auto zset = zset::ZSet::Create();
        for (size_t i = 0; i < elements; ++i) {
          // Commands store neither NaN nor -0 (ZADD reads "-0" as 0, as
          // Redis does), and a listpack keeps no sign for zero.
          double score = input->ReadScore();
          if (score != score || score == 0) {
            score = 0;
          }
          zset->InsertOrUpdate(ReadElement(input), score);
        }
        object = RedisObject::CreateWithZSet(std::move(zset));
        break;
      }
      default: {
        auto hash = hash::Hash::Create();
        for (size_t i = 0; i < elements; ++i) {
          hash->Set(ReadElement(input), ReadElement(input));
        }
        object = RedisObject::CreateWithHash(std::move(hash));
        break;
      }
    }
    db->SetKey(key, std::move(object),
               input->ReadByte() % 3 == 0 ? kFuture : 0);
  }
}

bool LoadFile(const std::string& path, db::RedisDb* db) {
  const int fd = aof::OpenFile(path, O_RDONLY);
  if (fd < 0) {
    return false;
  }
  rdb::LoadStats stats;
  const bool loaded = rdb::LoadRdb(fd, db, {.now_ms = 0}, &stats);
  close(fd);
  return loaded;
}

bool WriteFile(const std::string& path, std::string_view contents) {
  const int fd = aof::OpenFile(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return false;
  }
  const bool written =
      aof::WriteAll(aof::DefaultFileOps().get(), fd, contents) ==
      static_cast<ssize_t>(contents.size());
  close(fd);
  return written;
}

std::optional<std::string> ReadFile(const std::string& path) {
  return aof::ReadWholeFile(path);
}
}  // namespace

void FuzzRdb(const uint8_t* data, size_t size) {
  std::array<char, 32> name{};
  std::ranges::copy(std::string_view("/tmp/redis_rdb_XXXXXX"), name.begin());
  const int fd = mkstemp(name.data());
  if (fd < 0) {
    return;
  }
  close(fd);
  const std::string path(name.data());

  // The raw bytes as a file: anything may be refused, nothing may crash.
  if (WriteFile(path,
                std::string_view(reinterpret_cast<const char*>(data), size))) {
    auto loaded = db::RedisDb::Create();
    LoadFile(path, loaded.get());
  }

  // A database built from the bytes loads back the same.
  FuzzInput input(data, size);
  auto db = db::RedisDb::Create();
  Populate(&input, db.get());
  const bool checksum = input.ReadByte() % 4 != 0;
  const rdb::WriteOptions options{.compression = input.ReadByte() % 4 != 0,
                                  .checksum = checksum};
  Require(
      rdb::WriteRdbFile(db.get(), path, options, aof::DefaultFileOps().get()) ==
      rdb::WriteStatus::kWritten);
  auto loaded = db::RedisDb::Create();
  Require(LoadFile(path, loaded.get()));
  Require(Snapshot(loaded.get()) == Snapshot(db.get()));

  // Changed bytes are rejected while a checksum covers them, and never
  // crash the loader.
  const std::optional<std::string> written = ReadFile(path);
  if (written.has_value() && !written->empty() && input.HasData()) {
    std::string changed = *written;
    for (size_t changes = 1 + (input.ReadByte() % 3); changes > 0; --changes) {
      const size_t offset = input.ReadIndex(changed.size());
      changed[offset] =
          static_cast<char>(changed[offset] ^ (1 + (input.ReadByte() % 255)));
    }
    if (WriteFile(path, changed)) {
      auto corrupted = db::RedisDb::Create();
      const bool accepted = LoadFile(path, corrupted.get());
      Require(!checksum || !accepted || changed == *written);
    }
  }
  unlink(path.c_str());
}
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzRdb(data, size);
  return 0;
}
