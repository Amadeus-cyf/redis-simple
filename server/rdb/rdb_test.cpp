#include "server/rdb/rdb.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "data_types/hash/hash.h"
#include "data_types/list/list.h"
#include "data_types/set/set.h"
#include "data_types/zset/zset.h"
#include "memory/listpack.h"
#include "server/aof/file_ops.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/rdb/rdb_loader.h"
#include "server/rdb/rdb_writer.h"
#include "utils/crc64.h"
#include "utils/float_utils.h"

namespace redis_simple::rdb {
namespace {
using db::RedisObject;

// A far-future expiration, so keys do not expire during the test.
constexpr int64_t kFuture = int64_t{4102444800000};

// Describes a key's type, encoding, contents, and expiration as text, in an
// order that does not depend on hash tables: lists and sorted sets in
// order, sets and hashes sorted.
std::string Describe(db::RedisDb* db, std::string_view key,
                     const RedisObject& object) {
  std::string out;
  switch (object.Type()) {
    case RedisObject::ObjectType::kString:
      out = "string:" + std::string(object.String().View());
      break;
    case RedisObject::ObjectType::kList: {
      const list::List* list = object.List();
      out = list->Encoding() == list::List::Encoding::kListPack
                ? "list(listpack):"
                : "list(quicklist):";
      list->ForEach(0, list->Size() - 1, [&out](std::string_view value) {
        out.append(value).push_back(',');
        return true;
      });
      break;
    }
    case RedisObject::ObjectType::kSet: {
      const set::Set* set = object.Set();
      std::vector<std::string> members;
      set->ForEachMember([&members](std::string_view member) {
        members.emplace_back(member);
        return true;
      });
      std::ranges::sort(members);
      out = "set(" + std::to_string(static_cast<int>(set->Encoding())) + "):";
      for (const std::string& member : members) {
        out.append(member).push_back(',');
      }
      break;
    }
    case RedisObject::ObjectType::kZSet: {
      const zset::ZSet* zset = object.ZSet();
      out = zset->ListPackBytes().has_value() ? "zset(listpack):"
                                              : "zset(skiplist):";
      zset->ForEachEntry([&out](std::string_view member, double score) {
        out.append(member).push_back('=');
        out.append(utils::FloatToString(score)).push_back(',');
        return true;
      });
      break;
    }
    case RedisObject::ObjectType::kHash: {
      const hash::Hash* hash = object.Hash();
      std::vector<std::string> entries;
      hash->ForEachEntry(
          [&entries](std::string_view field, std::string_view value) {
            entries.push_back(std::string(field) + "=" + std::string(value));
            return true;
          });
      std::ranges::sort(entries);
      out = hash->Encoding() == hash::Hash::Encoding::kListPack
                ? "hash(listpack):"
                : "hash(dict):";
      for (const std::string& entry : entries) {
        out.append(entry).push_back(',');
      }
      break;
    }
  }
  if (const std::optional<int64_t> expire = db->Expiration(key)) {
    out += " expire=" + std::to_string(*expire);
  }
  return out;
}

std::map<std::string, std::string> DescribeAll(db::RedisDb* db) {
  std::map<std::string, std::string> keys;
  db->ForEachObject(
      [db, &keys](std::string_view key, const RedisObject& object) {
        keys.emplace(key, Describe(db, key, object));
        return true;
      });
  return keys;
}

class RdbTest : public testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("rdb_test_" + std::to_string(getpid()));
    std::filesystem::create_directories(directory_);
    path_ = (directory_ / "dump.rdb").string();
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }

  // Fills a database with every type in each encoding this server uses.
  static void Populate(db::RedisDb* db) {
    db->SetKey("int", RedisObject::CreateWithString("42"), 0);
    db->SetKey("int64", RedisObject::CreateWithString("-9223372036854775808"),
               0);
    db->SetKey("short", RedisObject::CreateWithString("hello"), kFuture);
    db->SetKey("empty", RedisObject::CreateWithString(""), 0);
    std::string compressible;
    for (int i = 0; i < 100; ++i) {
      compressible += "abcabc";
    }
    db->SetKey("compressible", RedisObject::CreateWithString(compressible), 0);
    std::string random;
    uint32_t state = 1;
    for (int i = 0; i < 500; ++i) {
      state = (state * 1103515245) + 12345;
      random.push_back(static_cast<char>(state >> 16));
    }
    db->SetKey("random", RedisObject::CreateWithString(random), 0);
    // Larger than a write block, so it goes to the file directly.
    db->SetKey("large",
               RedisObject::CreateWithString(std::string(3 << 20, 'L')), 0);

    auto small_list = list::List::Create();
    for (const char* value : {"a", "1", "-5", "300", "bbb"}) {
      small_list->RPush(value);
    }
    db->SetKey("list:small", RedisObject::CreateWithList(std::move(small_list)),
               0);
    auto big_list = list::List::Create();
    for (int i = 0; i < 2000; ++i) {
      big_list->RPush("element-" + std::to_string(i));
    }
    // A two-byte backlen at Redis' boundary, and an element too big for a
    // node of its own.
    big_list->RPush(std::string(16378, 'b'));
    big_list->RPush(std::string(100000, 'p'));
    db->SetKey("list:big", RedisObject::CreateWithList(std::move(big_list)),
               kFuture);

    auto ints = set::Set::Create();
    for (const char* value :
         {"3", "1", "-2", "100000", "-9223372036854775808"}) {
      ints->Add(value);
    }
    db->SetKey("set:ints", RedisObject::CreateWithSet(std::move(ints)), 0);
    auto strings = set::Set::Create();
    for (const char* value : {"b", "a", "c", "10"}) {
      strings->Add(value);
    }
    db->SetKey("set:strings", RedisObject::CreateWithSet(std::move(strings)),
               0);
    auto big_set = set::Set::Create();
    for (int i = 0; i < 300; ++i) {
      big_set->Add("member-" + std::to_string(i));
    }
    db->SetKey("set:big", RedisObject::CreateWithSet(std::move(big_set)), 0);

    auto small_zset = zset::ZSet::Create();
    small_zset->InsertOrUpdate("a", 1.5);
    small_zset->InsertOrUpdate("b", 2);
    small_zset->InsertOrUpdate("c", -3);
    small_zset->InsertOrUpdate("d", std::numeric_limits<double>::infinity());
    small_zset->InsertOrUpdate("e", 0.1);
    db->SetKey("zset:small", RedisObject::CreateWithZSet(std::move(small_zset)),
               0);
    auto big_zset = zset::ZSet::Create();
    for (int i = 0; i < 300; ++i) {
      big_zset->InsertOrUpdate("member-" + std::to_string(i), i / 3.0);
    }
    big_zset->InsertOrUpdate("ninf", -std::numeric_limits<double>::infinity());
    db->SetKey("zset:big", RedisObject::CreateWithZSet(std::move(big_zset)), 0);

    auto small_hash = hash::Hash::Create();
    small_hash->Set("f1", "v1");
    small_hash->Set("n", "12345");
    db->SetKey("hash:small", RedisObject::CreateWithHash(std::move(small_hash)),
               0);
    auto big_hash = hash::Hash::Create();
    for (int i = 0; i < 600; ++i) {
      big_hash->Set("field-" + std::to_string(i), "value-" + std::to_string(i));
    }
    db->SetKey("hash:big", RedisObject::CreateWithHash(std::move(big_hash)),
               kFuture);
  }

  WriteStatus Write(db::RedisDb* db, WriteOptions options = {}) {
    return WriteRdbFile(db, path_, options, aof::DefaultFileOps().get());
  }

  bool Load(db::RedisDb* db, LoadOptions options, LoadStats* stats) {
    const int fd = aof::OpenFile(path_, O_RDONLY);
    if (fd < 0) {
      return false;
    }
    const bool loaded = LoadRdb(fd, db, options, stats);
    close(fd);
    return loaded;
  }

  bool Load(db::RedisDb* db) {
    LoadStats stats;
    return Load(db, {}, &stats);
  }

  [[nodiscard]] std::string ReadFile() const {
    std::ifstream file(path_, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }

  void WriteFile(const std::string& contents) const {
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file << contents;
  }

 private:
  std::filesystem::path directory_;
  std::string path_;
};

TEST_F(RdbTest, RoundTripsEveryTypeAndEncoding) {
  auto db = db::RedisDb::Create();
  Populate(db.get());
  ASSERT_EQ(Write(db.get()), WriteStatus::kWritten);
  EXPECT_TRUE(ReadFile().starts_with("REDIS0011"));

  auto loaded = db::RedisDb::Create();
  LoadStats stats;
  ASSERT_TRUE(Load(loaded.get(), {}, &stats));
  EXPECT_EQ(stats.keys_loaded, db->KeyCount());
  EXPECT_EQ(stats.bytes, ReadFile().size());
  // Same contents, encodings, and expirations.
  EXPECT_EQ(DescribeAll(loaded.get()), DescribeAll(db.get()));
}

TEST_F(RdbTest, RoundTripsWithoutCompressionOrChecksum) {
  auto db = db::RedisDb::Create();
  Populate(db.get());
  WriteOptions options;
  options.compression = false;
  options.checksum = false;
  ASSERT_EQ(Write(db.get(), options), WriteStatus::kWritten);
  // Without rdbchecksum the file ends with eight zero bytes.
  const std::string contents = ReadFile();
  EXPECT_EQ(contents.substr(contents.size() - 8), std::string(8, '\0'));
  auto loaded = db::RedisDb::Create();
  ASSERT_TRUE(Load(loaded.get()));
  EXPECT_EQ(DescribeAll(loaded.get()), DescribeAll(db.get()));
}

TEST_F(RdbTest, WritesAnEmptyDatabase) {
  auto db = db::RedisDb::Create();
  ASSERT_EQ(Write(db.get()), WriteStatus::kWritten);
  auto loaded = db::RedisDb::Create();
  ASSERT_TRUE(Load(loaded.get()));
  EXPECT_EQ(loaded->KeyCount(), 0);
}

TEST_F(RdbTest, SkipsExpiredKeysUnlessAsked) {
  auto db = db::RedisDb::Create();
  db->SetKey("live", RedisObject::CreateWithString("v"), kFuture);
  db->SetKey("old", RedisObject::CreateWithString("v"), kFuture);
  ASSERT_EQ(Write(db.get()), WriteStatus::kWritten);

  // Loading after both expire, then after only "old" would have.
  auto later = db::RedisDb::Create();
  LoadStats stats;
  ASSERT_TRUE(Load(later.get(), {.now_ms = kFuture + 1}, &stats));
  EXPECT_EQ(later->KeyCount(), 0);
  EXPECT_EQ(stats.keys_expired, 2);

  // An AOF base keeps them for the records that follow.
  auto base = db::RedisDb::Create();
  ASSERT_TRUE(
      Load(base.get(), {.now_ms = kFuture + 1, .keep_expired = true}, &stats));
  EXPECT_EQ(stats.keys_loaded, 2);
  EXPECT_EQ(stats.keys_expired, 0);
}

TEST_F(RdbTest, RejectsEveryTruncationAndByteChange) {
  auto db = db::RedisDb::Create();
  db->SetKey("s", RedisObject::CreateWithString("value"), kFuture);
  auto hash = hash::Hash::Create();
  hash->Set("field", "value");
  // LLVM 18 cannot trace unique_ptr ownership through Dict::FindOrInsert.
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  db->SetKey("h", RedisObject::CreateWithHash(std::move(hash)), 0);
  auto list = list::List::Create();
  list->RPush("element");
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  db->SetKey("l", RedisObject::CreateWithList(std::move(list)), 0);
  ASSERT_EQ(Write(db.get()), WriteStatus::kWritten);
  const std::string contents = ReadFile();

  for (size_t size = 0; size < contents.size(); ++size) {
    WriteFile(contents.substr(0, size));
    auto loaded = db::RedisDb::Create();
    EXPECT_FALSE(Load(loaded.get())) << "truncated to " << size;
  }
  // The checksum covers every byte, so any change is caught.
  for (size_t offset = 0; offset < contents.size(); ++offset) {
    std::string changed = contents;
    changed[offset] = static_cast<char>(changed[offset] ^ 0x5a);
    WriteFile(changed);
    auto loaded = db::RedisDb::Create();
    EXPECT_FALSE(Load(loaded.get())) << "byte " << offset;
  }
}

TEST_F(RdbTest, RejectsUnsupportedTypesAndDatabases) {
  const std::string header = "REDIS0011";
  const std::string zero_checksum(8, '\0');
  // A stream (type 21), which this server does not have.
  WriteFile(header + std::string("\x15\x01k", 3) + "\xff" + zero_checksum);
  auto db = db::RedisDb::Create();
  EXPECT_FALSE(Load(db.get()));
  // Keys in database 1.
  WriteFile(header + std::string("\xfe\x01\x00\x01k\x01v\xff", 8) +
            zero_checksum);
  EXPECT_FALSE(Load(db.get()));
  // A newer format version.
  WriteFile("REDIS0016\xff" + zero_checksum);
  EXPECT_FALSE(Load(db.get()));
  // A version this server reads, with a string key in database 0.
  WriteFile(header + std::string("\xfe\x00\x00\x01k\x01v\xff", 8) +
            zero_checksum);
  ASSERT_TRUE(Load(db.get()));
  EXPECT_EQ(db->KeyCount(), 1);
  // Loading it again finds the key twice.
  EXPECT_FALSE(Load(db.get()));
}

// A listpack element as Redis encodes it, for listpacks that differ from
// what ListPack writes: a 7-bit unsigned integer, or a string of up to 63
// bytes even when its text is an integer.
struct RawElement {
  std::string_view string;
  int integer;
  bool is_integer;
};

std::string RawListPack(const std::vector<RawElement>& elements) {
  std::string body;
  for (const RawElement& element : elements) {
    if (element.is_integer) {
      body.push_back(static_cast<char>(element.integer));
      body.push_back('\x01');
    } else {
      body.push_back(static_cast<char>(0x80 | element.string.size()));
      body.append(element.string);
      body.push_back(static_cast<char>(1 + element.string.size()));
    }
  }
  const size_t total = 6 + body.size() + 1;
  std::string listpack;
  for (int i = 0; i < 4; ++i) {
    listpack.push_back(static_cast<char>((total >> (8 * i)) & 0xff));
  }
  listpack.push_back(static_cast<char>(elements.size() & 0xff));
  listpack.push_back(static_cast<char>(elements.size() >> 8));
  return listpack + body + '\xff';
}

RawElement S(std::string_view string) { return {string, 0, false}; }
RawElement I(int integer) { return {{}, integer, true}; }

// An RDB file with one key in database 0 holding `blob` as `type`, without
// a checksum.
std::string FileWithValue(uint8_t type, const std::string& blob) {
  std::string file = "REDIS0011";
  file.append("\xfe\x00", 2);
  file.push_back(static_cast<char>(type));
  file.append("\x01k");
  // A 14-bit RDB length.
  file.push_back(static_cast<char>(0x40 | (blob.size() >> 8)));
  file.push_back(static_cast<char>(blob.size() & 0xff));
  file += blob;
  file.push_back('\xff');
  return file + std::string(8, '\0');
}

constexpr uint8_t kHashListPackType = 16;
constexpr uint8_t kZSetListPackType = 17;
constexpr uint8_t kSetListPackType = 20;

// Listpacks that fit this server's limits are kept as they are, as Redis
// keeps them, and work as values afterwards.
TEST_F(RdbTest, KeepsTheBytesOfCompactValues) {
  auto db = db::RedisDb::Create();
  Populate(db.get());
  ASSERT_EQ(Write(db.get()), WriteStatus::kWritten);
  auto loaded = db::RedisDb::Create();
  ASSERT_TRUE(Load(loaded.get()));
  const auto object = [&loaded](std::string_view key) {
    return loaded->MutableLookupKey(key);
  };
  EXPECT_EQ(object("set:strings")->Set()->ListPackBytes(),
            db->MutableLookupKey("set:strings")->Set()->ListPackBytes());
  EXPECT_EQ(object("hash:small")->Hash()->ListPackBytes(),
            db->MutableLookupKey("hash:small")->Hash()->ListPackBytes());
  EXPECT_EQ(object("zset:small")->ZSet()->ListPackBytes(),
            db->MutableLookupKey("zset:small")->ZSet()->ListPackBytes());

  set::Set* set = object("set:strings")->Set();
  EXPECT_FALSE(set->Add("10"));
  EXPECT_TRUE(set->Add("d"));
  EXPECT_TRUE(set->HasMember("a"));
  hash::Hash* hash = object("hash:small")->Hash();
  EXPECT_FALSE(hash->Set("n", "6"));
  EXPECT_EQ(hash->Get("n"), "6");
  zset::ZSet* zset = object("zset:small")->ZSet();
  EXPECT_TRUE(zset->InsertOrUpdate("z", 1.75));
  EXPECT_EQ(zset->Rank("z"), 3);  // After c=-3, e=0.1, and a=1.5.
  EXPECT_EQ(zset->Score("e"), 0.1);
}

// Valid listpacks this server would encode differently are rebuilt: an
// integer stored as a string, sorted set pairs out of order, and more
// entries than its listpacks hold.
TEST_F(RdbTest, RebuildsListPacksEncodedDifferently) {
  auto db = db::RedisDb::Create();
  WriteFile(FileWithValue(kSetListPackType, RawListPack({S("7"), S("a")})));
  ASSERT_TRUE(Load(db.get()));
  EXPECT_TRUE(db->MutableLookupKey("k")->Set()->HasMember("7"));
  EXPECT_EQ(Describe(db.get(), "k", *db->MutableLookupKey("k")), "set(1):7,a,");

  db = db::RedisDb::Create();
  WriteFile(FileWithValue(
      kZSetListPackType,
      RawListPack({S("b"), I(2), S("a"), I(1), S("c"), S("1.5")})));
  ASSERT_TRUE(Load(db.get()));
  EXPECT_EQ(Describe(db.get(), "k", *db->MutableLookupKey("k")),
            "zset(listpack):a=1,c=1.5,b=2,");

  // Equal scores order members as bytes.
  db = db::RedisDb::Create();
  WriteFile(FileWithValue(kZSetListPackType,
                          RawListPack({S("b"), I(1), S("a"), I(1)})));
  ASSERT_TRUE(Load(db.get()));
  EXPECT_EQ(Describe(db.get(), "k", *db->MutableLookupKey("k")),
            "zset(listpack):a=1,b=1,");

  memory::ListPack big;
  for (size_t i = 0; i <= hash::Hash::kListPackMaxEntries; ++i) {
    big.Append("f" + std::to_string(i));
    big.Append("v");
  }
  db = db::RedisDb::Create();
  WriteFile(FileWithValue(kHashListPackType, std::string(big.Bytes())));
  ASSERT_TRUE(Load(db.get()));
  EXPECT_EQ(db->MutableLookupKey("k")->Hash()->Encoding(),
            hash::Hash::Encoding::kDict);
  EXPECT_EQ(db->MutableLookupKey("k")->Hash()->Size(),
            hash::Hash::kListPackMaxEntries + 1);
}

TEST_F(RdbTest, RejectsInvalidCompactValues) {
  const std::vector<std::pair<uint8_t, std::string>> cases = {
      {kSetListPackType, RawListPack({S("a"), S("a")})},
      {kSetListPackType, RawListPack({I(1), S("b"), I(1)})},
      // The same member, once as an integer and once as text.
      {kSetListPackType, RawListPack({I(7), S("7")})},
      {kHashListPackType, RawListPack({S("f"), S("1"), S("f"), S("2")})},
      {kHashListPackType, RawListPack({S("f"), S("1"), S("g")})},
      {kZSetListPackType, RawListPack({S("a"), I(1), S("a"), I(2)})},
      {kZSetListPackType, RawListPack({S("a"), S("x")})},
      {kZSetListPackType, RawListPack({S("a"), S("nan")})},
  };
  for (const auto& [type, blob] : cases) {
    WriteFile(FileWithValue(type, blob));
    auto db = db::RedisDb::Create();
    EXPECT_FALSE(Load(db.get())) << static_cast<int>(type);
  }
}

// A file Redis 8.10 wrote with SAVE, holding every type in each of its
// encodings, LZF-compressed strings, expirations, and a key that had
// expired.
TEST_F(RdbTest, LoadsAFileRedisWrote) {
  const std::string fixture = (std::filesystem::path(__FILE__).parent_path() /
                               "testdata" / "redis_8_10.rdb")
                                  .string();
  const int fd = aof::OpenFile(fixture, O_RDONLY);
  ASSERT_GE(fd, 0) << fixture;
  auto db = db::RedisDb::Create();
  LoadStats stats;
  ASSERT_TRUE(LoadRdb(fd, db.get(), {.now_ms = kFuture - 1}, &stats));
  close(fd);
  EXPECT_EQ(stats.keys_loaded, 19);
  EXPECT_EQ(stats.keys_expired, 1);

  const std::map<std::string, std::string> keys = DescribeAll(db.get());
  EXPECT_EQ(keys.at("int:small"), "string:42");
  EXPECT_EQ(keys.at("int:big"), "string:9223372036854775807");
  EXPECT_EQ(keys.at("str:short"), "string:hello");
  EXPECT_EQ(keys.at("str:empty"), "string:");
  std::string compressible;
  for (int i = 0; i < 20; ++i) {
    compressible += "abcabcabc";
  }
  EXPECT_EQ(keys.at("str:compressible"), "string:" + compressible);
  const std::string random = keys.at("str:random").substr(7);
  EXPECT_EQ(random.size(), 300);
  EXPECT_TRUE(random.starts_with("ieqh524yng5by1a2rogubbb8ayn1b7o2"));
  EXPECT_EQ(keys.at("list:small"), "list(listpack):a,1,-5,300,bbb,");
  std::string big_list = "list(quicklist):";
  for (int i = 1; i <= 600; ++i) {
    big_list +=
        "element-" + std::to_string(i) + "-" + std::string(20, 'x') + ",";
  }
  EXPECT_EQ(keys.at("list:big"), big_list);
  EXPECT_EQ(keys.at("set:ints"), "set(0):-2,1,100000,3,");
  EXPECT_EQ(keys.at("set:strings"), "set(1):10,a,b,c,");
  EXPECT_TRUE(keys.at("set:big").starts_with("set(2):member-1,member-10,"));
  EXPECT_TRUE(keys.at("set:bigints").starts_with("set(2):1,10,100,"));
  EXPECT_EQ(keys.at("zset:small"), "zset(listpack):c=-3,a=1.5,b=2,d=inf,");
  EXPECT_TRUE(keys.at("zset:big")
                  .starts_with("zset(skiplist):member-1=1.25,member-2=2.25,"));
  EXPECT_EQ(keys.at("hash:small"), "hash(listpack):f1=v1,n=12345,");
  EXPECT_EQ(keys.at("hash:longvalue"),
            "hash(dict):f=" + std::string(100, 'v') + ",");
  // Sorted as text, "field-100=" comes first.
  EXPECT_TRUE(
      keys.at("hash:big").starts_with("hash(dict):field-100=value-100,"));
  EXPECT_EQ(keys.at("expiring:future"), "string:v expire=4102444800000");
  EXPECT_EQ(keys.at("expiring:hash"),
            "hash(listpack):a=b, expire=4102444800000");
  EXPECT_EQ(keys.count("expired:past"), 0);
}

// File operations whose renames fail on request, to make saves fail.
class FailableRenames final : public aof::FileOps {
 public:
  ssize_t Write(int fd, const void* data, size_t size) override {
    return write(fd, data, size);
  }
  ssize_t WriteVector(int fd, const iovec* blocks, int count) override {
    return writev(fd, blocks, count);
  }
  int Sync(int fd) override { return fsync(fd); }
  int Rename(const char* source, const char* destination) override {
    if (fail_renames) {
      errno = EIO;
      return -1;
    }
    return std::rename(source, destination);
  }

  bool fail_renames{false};
};

// Rdb's background saves fork, so they run in a directory of their own,
// which becomes the working directory, as the server's --dir does.
class RdbBackgroundSaveTest : public testing::Test {
 protected:
  void SetUp() override {
    previous_directory_ = std::filesystem::current_path();
    directory_ = std::filesystem::temp_directory_path() /
                 ("rdb_background_test_" + std::to_string(getpid()));
    std::filesystem::create_directories(directory_);
    std::filesystem::current_path(directory_);
    db_ = db::RedisDb::Create();
    db_->SetKey("key", RedisObject::CreateWithString("value"), 0);
  }

  void TearDown() override {
    std::filesystem::current_path(previous_directory_);
    std::filesystem::remove_all(directory_);
  }

  std::unique_ptr<Rdb> Create(Options options = {}) {
    options.file_ops = file_ops_;
    return Rdb::Create(std::move(options));
  }

  // Runs the cron until the background save's child has exited.
  void FinishBackgroundSave(Rdb* rdb, bool may_fork = true) {
    for (int attempt = 0; attempt < 500 && rdb->BackgroundSaveInProgress();
         ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      rdb->Cron(db_.get(), may_fork);
    }
    ASSERT_FALSE(rdb->BackgroundSaveInProgress());
  }

  // Files left in the directory besides dump.rdb, such as temporary files.
  [[nodiscard]] std::vector<std::string> OtherFiles() const {
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
      if (entry.path().filename() != "dump.rdb") {
        files.push_back(entry.path().filename().string());
      }
    }
    return files;
  }

  db::RedisDb* Db() { return db_.get(); }
  FailableRenames* FileOps() { return file_ops_.get(); }

 private:
  std::filesystem::path previous_directory_;
  std::filesystem::path directory_;
  std::unique_ptr<db::RedisDb> db_;
  std::shared_ptr<FailableRenames> file_ops_ =
      std::make_shared<FailableRenames>();
};

TEST_F(RdbBackgroundSaveTest, SavesInAChildAndCountsChangesMadeMeanwhile) {
  auto rdb = Create();
  rdb->AddChanges(5);
  ASSERT_EQ(rdb->StartBackgroundSave(Db()), BackgroundSaveResult::kStarted);
  EXPECT_TRUE(rdb->BackgroundSaveInProgress());
  EXPECT_TRUE(rdb->State().background_save_in_progress);
  EXPECT_GE(rdb->State().current_background_save_seconds, 0);
  EXPECT_EQ(rdb->StartBackgroundSave(Db()), BackgroundSaveResult::kInProgress);
  // Made while the child saves, so not in the file.
  rdb->AddChanges(2);
  FinishBackgroundSave(rdb.get());

  const RdbState state = rdb->State();
  EXPECT_EQ(state.changes_since_last_save, 2);
  EXPECT_EQ(state.saves, 1);
  EXPECT_TRUE(state.last_background_save_ok);
  EXPECT_EQ(state.consecutive_failures, 0);
  EXPECT_GE(state.last_background_save_seconds, 0);
  EXPECT_EQ(state.current_background_save_seconds, -1);
  EXPECT_FALSE(rdb->DeniesWrites());
  EXPECT_TRUE(OtherFiles().empty());

  auto loaded = db::RedisDb::Create();
  ASSERT_TRUE(Create()->Load(loaded.get()));
  EXPECT_EQ(DescribeAll(loaded.get()), DescribeAll(Db()));
}

TEST_F(RdbBackgroundSaveTest, FailedSaveStopsWritesUntilASaveSucceeds) {
  auto rdb = Create();
  rdb->AddChanges(3);
  FileOps()->fail_renames = true;
  ASSERT_EQ(rdb->StartBackgroundSave(Db()), BackgroundSaveResult::kStarted);
  FinishBackgroundSave(rdb.get());
  RdbState state = rdb->State();
  EXPECT_FALSE(state.last_background_save_ok);
  EXPECT_EQ(state.consecutive_failures, 1);
  EXPECT_EQ(state.changes_since_last_save, 3);
  EXPECT_TRUE(rdb->DeniesWrites());
  // The child removed its temporary file.
  EXPECT_TRUE(OtherFiles().empty());

  // SAVE succeeding clears the error, as Redis' rdbSave does.
  FileOps()->fail_renames = false;
  ASSERT_TRUE(rdb->Save(Db()));
  state = rdb->State();
  EXPECT_TRUE(state.last_background_save_ok);
  EXPECT_EQ(state.changes_since_last_save, 0);
  EXPECT_EQ(state.saves, 2);
  EXPECT_FALSE(rdb->DeniesWrites());
}

// Options without save points, as --save "" sets them.
Options WithoutSavePoints() {
  Options options;
  options.save_points.clear();
  return options;
}

TEST_F(RdbBackgroundSaveTest, FailedSaveAllowsWritesWithoutSavePoints) {
  Options keep_writing;
  keep_writing.stop_writes_on_error = false;
  for (const Options& options : {WithoutSavePoints(), keep_writing}) {
    auto rdb = Create(options);
    FileOps()->fail_renames = true;
    ASSERT_EQ(rdb->StartBackgroundSave(Db()), BackgroundSaveResult::kStarted);
    FinishBackgroundSave(rdb.get());
    EXPECT_FALSE(rdb->State().last_background_save_ok);
    EXPECT_FALSE(rdb->DeniesWrites());
  }
}

TEST_F(RdbBackgroundSaveTest, ScheduledSaveWaitsForOtherChildren) {
  auto rdb = Create();
  rdb->ScheduleBackgroundSave();
  // An AOF rewrite runs: no second child.
  rdb->Cron(Db(), false);
  EXPECT_FALSE(rdb->BackgroundSaveInProgress());
  rdb->Cron(Db(), true);
  EXPECT_TRUE(rdb->BackgroundSaveInProgress());
  FinishBackgroundSave(rdb.get());
  // Started once.
  rdb->Cron(Db(), true);
  EXPECT_FALSE(rdb->BackgroundSaveInProgress());
  EXPECT_EQ(rdb->State().saves, 1);
}

TEST_F(RdbBackgroundSaveTest, SavesBeforeShutdownOnlyWithSavePoints) {
  auto without = Create(WithoutSavePoints());
  without->AddChanges(1);
  ASSERT_TRUE(without->SaveBeforeShutdown(Db()));
  EXPECT_FALSE(std::filesystem::exists("dump.rdb"));

  auto rdb = Create();
  rdb->AddChanges(1);
  // A running background save is stopped first, so it cannot replace the
  // file the final save writes.
  ASSERT_EQ(rdb->StartBackgroundSave(Db()), BackgroundSaveResult::kStarted);
  ASSERT_TRUE(rdb->SaveBeforeShutdown(Db()));
  EXPECT_FALSE(rdb->BackgroundSaveInProgress());
  EXPECT_TRUE(std::filesystem::exists("dump.rdb"));
  EXPECT_TRUE(OtherFiles().empty());
  EXPECT_EQ(rdb->State().changes_since_last_save, 0);

  // A failed final save keeps the server running.
  FileOps()->fail_renames = true;
  EXPECT_FALSE(rdb->SaveBeforeShutdown(Db()));
}
}  // namespace
}  // namespace redis_simple::rdb
