#include "containers/listpack_reader.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "containers/listpack.h"
#include "containers/listpack_test_util.h"

namespace redis_simple::containers {
namespace {
// Reads every element as text, or nullopt if the reader reports corruption.
std::optional<std::vector<std::string>> ReadAll(std::string_view blob) {
  ListpackReader reader(blob);
  std::vector<std::string> elements;
  ListpackReader::Element element;
  while (true) {
    switch (reader.Next(&element)) {
      case ListpackReader::Status::kElement:
        elements.push_back(element.is_integer ? std::to_string(element.integer)
                                              : std::string(element.string));
        break;
      case ListpackReader::Status::kEnd:
        return elements;
      case ListpackReader::Status::kCorrupt:
        return std::nullopt;
    }
  }
}

std::vector<std::string> SampleElements() {
  std::vector<std::string> elements = {"",
                                       "a",
                                       "0",
                                       "127",
                                       "128",
                                       "-1",
                                       "4095",
                                       "-4096",
                                       "32767",
                                       "-32768",
                                       "8388607",
                                       "-8388608",
                                       "2147483647",
                                       "-2147483648",
                                       "9223372036854775807",
                                       "-9223372036854775808",
                                       "007",
                                       "+1"};
  for (const size_t length : {63, 64, 126, 127, 200, 4095, 4096, 20000}) {
    elements.emplace_back(length, 'x');
  }
  return elements;
}

TEST(ListpackReaderTest, ReadsWhatListpackWrites) {
  Listpack listpack;
  const std::vector<std::string> elements = SampleElements();
  for (const std::string& element : elements) {
    listpack.Append(element);
  }
  EXPECT_EQ(ReadAll(listpack.Bytes()), elements);
  EXPECT_EQ(ListpackReader(listpack.Bytes()).HeaderCount(), 0);
  Listpack empty;
  EXPECT_EQ(ReadAll(empty.Bytes()), std::vector<std::string>{});
}

TEST(ListpackReaderTest, ReadsRedisBytes) {
  // RPUSH l 0 -4097 5000-byte-string, as Redis 8.12 encodes it.
  std::string blob = {'\0', '\0', '\0', '\0', '\x03', '\0'};
  blob += std::string("\x00\x01", 2);              // 0
  blob += std::string("\xf1\xff\xef\x03", 4);      // -4097
  blob += std::string("\xf0\x88\x13\x00\x00", 5);  // 5000 bytes
  blob += std::string(5000, 'q') + "\x27\x8d";
  blob.push_back('\xff');
  const auto total = static_cast<uint32_t>(blob.size());
  for (int i = 0; i < 4; ++i) {
    blob[i] = static_cast<char>((total >> (8 * i)) & 0xff);
  }
  EXPECT_EQ(ReadAll(blob),
            (std::vector<std::string>{"0", "-4097", std::string(5000, 'q')}));
}

TEST(ListpackReaderTest, RejectsEveryTruncationAndBadHeader) {
  Listpack listpack;
  for (const std::string& element : SampleElements()) {
    listpack.Append(element);
  }
  const std::string blob(listpack.Bytes());
  for (size_t size = 0; size < blob.size(); ++size) {
    EXPECT_EQ(ReadAll(std::string_view(blob).substr(0, size)), std::nullopt)
        << size;
  }
  std::string wrong_count = blob;
  wrong_count[4] = static_cast<char>(wrong_count[4] + 1);
  EXPECT_EQ(ReadAll(wrong_count), std::nullopt);
}

TEST(ListpackReaderTest, NeverReadsOutsideCorruptedBytes) {
  Listpack listpack;
  for (const std::string& element : SampleElements()) {
    listpack.Append(element);
  }
  const std::string blob(listpack.Bytes());
  // A fixed seed keeps failures reproducible.
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(11);
  for (int round = 0; round < 20000; ++round) {
    std::string corrupted = blob;
    for (int flips = 1 + static_cast<int>(rng() % 3); flips > 0; --flips) {
      corrupted[rng() % corrupted.size()] = static_cast<char>(rng());
    }
    // Any result is fine; sanitizers catch reads outside the buffer.
    ReadAll(corrupted);
  }
}

using listpack_test::HashListpack;

TEST(WidenCompactBacklensTest, WidensTheBacklensRedis810Wrote) {
  struct Case {
    size_t length;
    std::string_view compact;
    std::string_view usual;
  };
  for (const Case& test : {Case{16378, std::string_view("\x7f\xff", 2),
                                std::string_view("\x00\xff\xff", 3)},
                           Case{2097146, std::string_view("\x7f\xff\xff", 3),
                                std::string_view("\x00\xff\xff\xff", 4)}}) {
    // Redis 8.10's bytes, which a reader of the usual width rejects.
    std::string listpack = HashListpack(test.length, 'v', test.compact);
    EXPECT_FALSE(ReadAll(listpack).has_value());
    ASSERT_TRUE(WidenCompactBacklens(&listpack));
    // Redis 8.12's bytes.
    EXPECT_EQ(listpack, HashListpack(test.length, 'v', test.usual));
    EXPECT_EQ(ReadAll(listpack),
              (std::vector<std::string>{"f", std::string(test.length, 'v')}));
  }
}

TEST(WidenCompactBacklensTest, WidensEveryShortBacklenAmongOtherEntries) {
  // Two short backlens among usual ones, as Listpack would write the
  // elements but for those two bytes.
  const std::string big(16378, 'x');
  Listpack expected;
  for (const std::string& element :
       std::vector<std::string>{"a", big, "12345", big, "z"}) {
    ASSERT_TRUE(expected.Append(element));
  }
  std::string listpack(expected.Bytes());
  const std::string usual("\x00\xff\xff", 3);
  const std::string compact("\x7f\xff", 2);
  int replaced = 0;
  for (size_t offset = listpack.find(usual); offset != std::string::npos;
       offset = listpack.find(usual, offset + compact.size())) {
    listpack.replace(offset, usual.size(), compact);
    ++replaced;
  }
  ASSERT_EQ(replaced, 2);
  for (int shift = 0; shift < 32; shift += 8) {
    listpack[shift / 8] = static_cast<char>((listpack.size() >> shift) & 0xff);
  }
  ASSERT_TRUE(WidenCompactBacklens(&listpack));
  EXPECT_EQ(listpack, expected.Bytes());
}

TEST(WidenCompactBacklensTest, LeavesOtherBytesAlone) {
  // The usual widths: three bytes at the limit, and one byte ('i' is 105,
  // the value's encoded size) in a listpack too small for a short backlen.
  for (std::string listpack :
       {HashListpack(16378, 'v', std::string_view("\x00\xff\xff", 3)),
        HashListpack(100, 'v', "i")}) {
    const std::string before = listpack;
    EXPECT_FALSE(WidenCompactBacklens(&listpack));
    EXPECT_EQ(listpack, before);
  }
}

TEST(WidenCompactBacklensTest, DoesNotRepairCorruption) {
  const std::string compact("\x7f\xff", 2);
  const std::string good = HashListpack(16378, 'v', compact);
  std::vector<std::string> corrupt;
  // A wrong total size in the header.
  corrupt.push_back(good);
  corrupt.back()[0] = static_cast<char>(corrupt.back()[0] + 1);
  // No terminator.
  corrupt.push_back(good.substr(0, good.size() - 1));
  // A byte too many before the terminator.
  corrupt.push_back(HashListpack(16378, 'v', std::string("\x7f\xff\x00", 3)));
  // Not quite the short form: 7f fe.
  corrupt.push_back(HashListpack(16378, 'v', std::string("\x7f\xfe", 2)));
  // A string length that runs past the end: 65536 bytes more.
  corrupt.push_back(good);
  corrupt.back()[12] = '\x01';
  for (std::string& listpack : corrupt) {
    const std::string before = listpack;
    EXPECT_FALSE(WidenCompactBacklens(&listpack));
    EXPECT_EQ(listpack, before);
    EXPECT_FALSE(ReadAll(listpack).has_value());
  }
}
}  // namespace
}  // namespace redis_simple::containers
